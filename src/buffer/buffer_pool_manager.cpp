#include "buffer/buffer_pool_manager.h"

#include <future>
#include <sstream>

#include "common/crash_point.h"
#include "common/exception.h"
#include "recovery/log_manager.h"

namespace ontodb {

BufferPoolManager::BufferPoolManager(size_t pool_size, DiskManager* disk_manager, size_t replacer_k,
                                     LogManager* log_manager, IoStats* stats, TraceSink* trace,
                                     size_t shards)
    : pool_size_(pool_size)
    , disk_manager_(disk_manager)
    , replacer_k_(replacer_k)
    , log_manager_(log_manager)
    , stats_(stats)
    , trace_(trace)
    , scheduler_(std::make_unique<DiskScheduler>(disk_manager, trace)) {
  const size_t count = shards == 0 ? 1 : shards;
  for (size_t i = 0; i < count; i++) {
    // The first pool_size mod shards shards take one frame more.
    const size_t frames = pool_size / count + (i < pool_size % count ? 1 : 0);
    auto shard = std::make_unique<Shard>(frames, replacer_k, trace);
    for (size_t f = 0; f < frames; f++) {
      shard->free_list.push_back(static_cast<frame_id_t>(f));
    }
    shards_.push_back(std::move(shard));
  }
}

BufferPoolManager::~BufferPoolManager() = default;

namespace {

auto IndexSlots(size_t frames) -> size_t {
  size_t n = 16;
  while (n < frames * 4) {
    n <<= 1;
  }
  return n;
}

auto HashPage(page_id_t page_id) -> size_t {
  return static_cast<size_t>((static_cast<uint64_t>(page_id) * 0x9E3779B97F4A7C15ULL) >> 20);
}

constexpr uint64_t kEmpty = 0;
constexpr uint64_t kDeleted = 1;
constexpr int kFrameBits = 24;

}  // namespace

BufferPoolManager::Shard::Shard(size_t frame_count, size_t k, TraceSink* trace)
    : frames(frame_count), replacer(frame_count, k, trace), index(IndexSlots(frame_count)) {}

void BufferPoolManager::Shard::IndexInsert(page_id_t page_id, frame_id_t frame) {
  if (deleted_entries > index.size() / 4) {  // rebuild: readers that miss meanwhile take the slow path
    for (auto& slot : index) {
      slot.store(kEmpty, std::memory_order_relaxed);
    }
    deleted_entries = 0;
    for (const auto& [id, f] : page_table) {
      if (id != page_id) {
        IndexInsert(id, f);
      }
    }
  }
  const size_t mask = index.size() - 1;
  size_t i = HashPage(page_id) & mask;
  while (true) {
    const uint64_t e = index[i].load(std::memory_order_relaxed);
    if (e == kEmpty || e == kDeleted) {
      if (e == kDeleted) {
        deleted_entries--;
      }
      index[i].store(((static_cast<uint64_t>(page_id) + 1) << kFrameBits) | static_cast<uint64_t>(frame),
                     std::memory_order_release);
      return;
    }
    i = (i + 1) & mask;
  }
}

void BufferPoolManager::Shard::IndexErase(page_id_t page_id) {
  const size_t mask = index.size() - 1;
  size_t i = HashPage(page_id) & mask;
  for (size_t probes = 0; probes < index.size(); probes++) {
    const uint64_t e = index[i].load(std::memory_order_relaxed);
    if (e == kEmpty) {
      return;
    }
    if (e != kDeleted && (e >> kFrameBits) == static_cast<uint64_t>(page_id) + 1) {
      index[i].store(kDeleted, std::memory_order_release);
      deleted_entries++;
      return;
    }
    i = (i + 1) & mask;
  }
}

auto BufferPoolManager::Shard::IndexFind(page_id_t page_id) const -> frame_id_t {
  const size_t mask = index.size() - 1;
  size_t i = HashPage(page_id) & mask;
  for (size_t probes = 0; probes < index.size(); probes++) {
    const uint64_t e = index[i].load(std::memory_order_acquire);
    if (e == kEmpty) {
      return INVALID_FRAME_ID;
    }
    if (e != kDeleted && (e >> kFrameBits) == static_cast<uint64_t>(page_id) + 1) {
      return static_cast<frame_id_t>(e & ((1ULL << kFrameBits) - 1));
    }
    i = (i + 1) & mask;
  }
  return INVALID_FRAME_ID;
}

auto BufferPoolManager::ShardOf(page_id_t page_id) const -> Shard& {
  return *shards_[static_cast<size_t>(page_id < 0 ? 0 : page_id) % shards_.size()];
}

auto BufferPoolManager::NewPage(page_id_t* page_id) -> Page* {
  // One shard: take the frame first, so a full pool allocates nothing. Several: the id picks the
  // shard, so it is allocated first.
  page_id_t id = INVALID_PAGE_ID;
  if (shards_.size() > 1) {
    id = disk_manager_->AllocatePage();
  }
  Shard& shard = shards_.size() > 1 ? ShardOf(id) : *shards_[0];
  std::lock_guard<std::mutex> guard(shard.mu);
  frame_id_t frame_id = INVALID_FRAME_ID;
  if (!TakeFrame(shard, &frame_id)) {
    return nullptr;
  }
  if (shards_.size() == 1) {
    id = disk_manager_->AllocatePage();
  }
  Page& page = shard.frames[frame_id];
  page.Reset();
  page.SetPageId(id);
  page.IncrementPin();
  shard.page_table[id] = frame_id;
  shard.replacer.RecordAccess(frame_id);
  shard.replacer.SetEvictable(frame_id, false);
  if (shards_.size() > 1) {
    shard.IndexInsert(id, frame_id);
  }
  *page_id = id;
  Trace("new page " + std::to_string(id) + " in frame " + std::to_string(frame_id));
  return &page;
}

auto BufferPoolManager::FetchPage(page_id_t page_id) -> Page* {
  Shard& shard = ShardOf(page_id);
  if (shards_.size() > 1 && page_id >= 0) {
    // A resident page is pinned without the shard latch. The pin can land on a frame that was
    // evicted and refilled after the index was read; the page id says so, and the pin is undone.
    const frame_id_t f = shard.IndexFind(page_id);
    if (f != INVALID_FRAME_ID) {
      Page& page = shard.frames[static_cast<size_t>(f)];
      if (page.TryPin()) {
        if (page.GetPageId() == page_id) {
          page.MarkReferenced();
          if (stats_ != nullptr) {
            stats_->BufferHit();
          }
          return &page;
        }
        page.DecrementPin();
      }
    }
  }
  std::lock_guard<std::mutex> guard(shard.mu);
  if (page_id < 0 || shard.deleted.count(page_id) != 0) {
    return nullptr;
  }
  if (const auto it = shard.page_table.find(page_id); it != shard.page_table.end()) {
    Page& page = shard.frames[it->second];
    page.IncrementPin();
    shard.replacer.RecordAccess(it->second);
    shard.replacer.SetEvictable(it->second, false);
    if (stats_ != nullptr) {
      stats_->BufferHit();
    }
    Trace("hit page " + std::to_string(page_id));
    return &page;
  }
  frame_id_t frame_id = INVALID_FRAME_ID;
  if (!TakeFrame(shard, &frame_id)) {
    return nullptr;
  }
  Page& page = shard.frames[frame_id];
  page.Reset();
  page.SetPageId(page_id);
  try {
    RunIo(false, &page);
  } catch (...) {
    page.Reset();
    shard.free_list.push_back(frame_id);
    throw;
  }
  page.IncrementPin();
  shard.page_table[page_id] = frame_id;
  shard.replacer.RecordAccess(frame_id);
  shard.replacer.SetEvictable(frame_id, false);
  if (shards_.size() > 1) {
    shard.IndexInsert(page_id, frame_id);
  }
  if (stats_ != nullptr) {
    stats_->BufferMiss();
  }
  Trace("miss page " + std::to_string(page_id) + " into frame " + std::to_string(frame_id));
  return &page;
}

auto BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty) -> bool {
  Shard& shard = ShardOf(page_id);
  if (shards_.size() > 1 && page_id >= 0) {
    const frame_id_t f = shard.IndexFind(page_id);
    if (f != INVALID_FRAME_ID) {
      Page& page = shard.frames[static_cast<size_t>(f)];
      if (page.GetPageId() == page_id) {  // the caller holds a pin, so the frame cannot have moved
        if (is_dirty) {
          page.SetDirty(true);
        }
        return page.TryUnpin();
      }
    }
  }
  std::lock_guard<std::mutex> guard(shard.mu);
  const auto it = shard.page_table.find(page_id);
  if (it == shard.page_table.end()) {
    return false;
  }
  Page& page = shard.frames[it->second];
  if (page.GetPinCount() <= 0) {
    return false;
  }
  page.DecrementPin();
  if (is_dirty) {
    page.SetDirty(true);
  }
  if (page.GetPinCount() == 0) {
    shard.replacer.SetEvictable(it->second, true);
  }
  Trace("unpin page " + std::to_string(page_id) + (is_dirty ? " dirty" : ""));
  return true;
}

auto BufferPoolManager::FlushPage(page_id_t page_id) -> bool {
  Shard& shard = ShardOf(page_id);
  std::lock_guard<std::mutex> guard(shard.mu);
  const auto it = shard.page_table.find(page_id);
  if (it == shard.page_table.end()) {
    return false;
  }
  Page& page = shard.frames[it->second];
  if (page.IsDirty()) {
    WriteBack(&page);
  }
  return true;
}

void BufferPoolManager::FlushAllPages() {
  for (auto& shard : shards_) {
    std::lock_guard<std::mutex> guard(shard->mu);
    for (auto& page : shard->frames) {
      if (page.GetPageId() != INVALID_PAGE_ID && page.IsDirty()) {
        WriteBack(&page);
      }
    }
  }
}

auto BufferPoolManager::DeletePage(page_id_t page_id) -> bool {
  Shard& shard = ShardOf(page_id);
  std::lock_guard<std::mutex> guard(shard.mu);
  if (const auto it = shard.page_table.find(page_id); it != shard.page_table.end()) {
    Page& page = shard.frames[it->second];
    if (shards_.size() > 1 ? !page.TryClaim() : page.GetPinCount() > 0) {
      return false;
    }
    shard.replacer.Remove(it->second);
    shard.free_list.push_back(it->second);
    if (shards_.size() > 1) {
      shard.IndexErase(page_id);
    }
    page.Reset();  // a deleted page's bytes are discarded, not written
    shard.page_table.erase(it);
  }
  disk_manager_->DeallocatePage(page_id);
  shard.deleted.insert(page_id);
  Trace("delete page " + std::to_string(page_id));
  return true;
}

auto BufferPoolManager::GetPinnedFrameCount() const -> size_t {
  size_t pinned = 0;
  for (const auto& shard : shards_) {
    std::lock_guard<std::mutex> guard(shard->mu);
    for (const auto& page : shard->frames) {
      if (page.GetPinCount() > 0) {
        pinned++;
      }
    }
  }
  return pinned;
}

void BufferPoolManager::LogPageWrite(Page* page) {
  if (log_manager_ == nullptr) {
    return;
  }
  auto record = LogRecord::Page(page->GetPageId(), page->GetData() + kPageLsnSize);
  page->SetLSN(log_manager_->Append(&record));
}

auto BufferPoolManager::BeginGroup() -> lsn_t {
  return log_manager_ == nullptr ? INVALID_LSN : log_manager_->BeginGroup();
}

void BufferPoolManager::EndGroup(lsn_t token) {
  if (log_manager_ != nullptr) {
    log_manager_->EndGroup(token);
  }
}

auto BufferPoolManager::DebugString() const -> std::string {
  size_t pinned = 0;
  size_t dirty = 0;
  size_t in_use = 0;
  size_t free_frames = 0;
  size_t evictable = 0;
  std::ostringstream rows;
  for (const auto& shard : shards_) {
    std::lock_guard<std::mutex> guard(shard->mu);
    for (size_t i = 0; i < shard->frames.size(); i++) {
      const Page& page = shard->frames[i];
      if (page.GetPageId() == INVALID_PAGE_ID) {
        continue;
      }
      pinned += page.GetPinCount() > 0 ? 1 : 0;
      dirty += page.IsDirty() ? 1 : 0;
      rows << "frame " << i << ": page " << page.GetPageId() << ", pins " << page.GetPinCount()
           << ", " << (page.IsDirty() ? "dirty" : "clean") << ", pageLSN " << page.GetLSN() << '\n';
    }
    in_use += shard->page_table.size();
    free_frames += shard->free_list.size();
    evictable += shard->replacer.Size();
  }
  std::ostringstream out;
  out << "buffer pool: " << pool_size_ << " frames, lru_k " << replacer_k_ << ", " << in_use
      << " in use, " << free_frames << " free, " << pinned << " pinned, " << dirty << " dirty, "
      << evictable << " evictable\n"
      << rows.str();
  return out.str();
}

// The guards turn a null page (a full pool, or a deleted page) into an error
// at the call that asked for it. The latch is taken after FetchPage returns,
// outside the shard latch, so waiting for a page never blocks the whole pool.
auto BufferPoolManager::FetchPageRead(page_id_t page_id) -> ReadPageGuard {
  Page* page = FetchPage(page_id);
  if (page == nullptr) {
    throw StorageException("cannot fetch page " + std::to_string(page_id) + ": " +
                           NullReason(page_id));
  }
  return {this, page};
}

auto BufferPoolManager::FetchPageWrite(page_id_t page_id) -> WritePageGuard {
  Page* page = FetchPage(page_id);
  if (page == nullptr) {
    throw StorageException("cannot fetch page " + std::to_string(page_id) + ": " +
                           NullReason(page_id));
  }
  return {this, page};
}

auto BufferPoolManager::NewPageGuarded(page_id_t* page_id) -> WritePageGuard {
  Page* page = NewPage(page_id);
  if (page == nullptr) {
    throw StorageException("cannot allocate a page: every frame is pinned");
  }
  return {this, page};
}

auto BufferPoolManager::NullReason(page_id_t page_id) const -> std::string {
  const Shard& shard = ShardOf(page_id);
  std::lock_guard<std::mutex> guard(shard.mu);
  if (page_id < 0 || shard.deleted.count(page_id) != 0) {
    return "the page was deleted or never existed";
  }
  return "every frame is pinned or held by an open page group";
}

// A free frame first; otherwise evict, writing the victim back if dirty.
auto BufferPoolManager::TakeFrame(Shard& shard, frame_id_t* frame_id) -> bool {
  if (!shard.free_list.empty()) {
    *frame_id = shard.free_list.front();
    shard.free_list.pop_front();
    return true;
  }
  // A dirty page whose record an open page group holds back cannot be
  // written yet; set it aside and ask the replacer again.
  const lsn_t flushable = log_manager_ == nullptr ? 0 : log_manager_->FlushableLsn();
  const bool fast = shards_.size() > 1;
  if (fast) {
    // Hits that took no latch left only a mark and a pin count; bring the replacer up to date.
    for (size_t i = 0; i < shard.frames.size(); i++) {
      Page& page = shard.frames[i];
      if (page.GetPageId() == INVALID_PAGE_ID) {
        continue;
      }
      if (page.TakeReferenced()) {
        shard.replacer.RecordAccess(static_cast<frame_id_t>(i));
      }
      shard.replacer.SetEvictable(static_cast<frame_id_t>(i), page.GetPinCount() == 0);
    }
  }
  std::vector<frame_id_t> held;
  std::vector<frame_id_t> pinned;
  frame_id_t victim = INVALID_FRAME_ID;
  bool found = false;
  while (shard.replacer.Evict(&victim)) {
    Page& candidate = shard.frames[victim];
    if (log_manager_ != nullptr && candidate.IsDirty() && candidate.GetLSN() > flushable) {
      held.push_back(victim);
      continue;
    }
    if (fast && !candidate.TryClaim()) {  // someone pinned it after the scan
      pinned.push_back(victim);
      continue;
    }
    found = true;
    break;
  }
  for (const frame_id_t frame : pinned) {
    shard.replacer.RecordAccess(frame);
  }
  for (const frame_id_t frame : held) {
    shard.replacer.RecordAccess(frame);
    shard.replacer.SetEvictable(frame, true);
  }
  if (!found) {
    return false;
  }
  Page& page = shard.frames[victim];
  if (page.IsDirty()) {
    try {
      WriteBack(&page);
    } catch (...) {
      // The victim stays resident and dirty; give it back to the replacer.
      shard.replacer.RecordAccess(victim);
      shard.replacer.SetEvictable(victim, true);
      if (fast) {
        page.Release();
      }
      throw;
    }
  }
  Trace("evict page " + std::to_string(page.GetPageId()) + " from frame " + std::to_string(victim));
  if (fast) {
    shard.IndexErase(page.GetPageId());
  }
  shard.page_table.erase(page.GetPageId());
  *frame_id = victim;
  return true;
}

// The write-ahead rule: the log up to this page's pageLSN is durable before
// the page is. The crash point between the two lets a test stop there.
void BufferPoolManager::WriteBack(Page* page) {
  if (log_manager_ != nullptr && page->GetLSN() > log_manager_->GetPersistentLsn()) {
    log_manager_->Flush(page->GetLSN(), true);
  }
  CrashPoint::Reach("BufferPoolManager::FlushPage::before_disk_write");
  RunIo(true, page);
  page->SetDirty(false);
  Trace("flush page " + std::to_string(page->GetPageId()));
}

void BufferPoolManager::RunIo(bool is_write, Page* page) {
  DiskRequest request;
  request.is_write = is_write;
  request.page_id = page->GetPageId();
  request.data = page->GetData();
  auto done = request.callback.get_future();
  scheduler_->Schedule(std::move(request));
  done.get();  // rethrows the disk manager's error, if any
}

void BufferPoolManager::Trace(const std::string& message) const {
  if (trace_ != nullptr) {
    trace_->Event("bpm", message);
  }
}

}  // namespace ontodb
