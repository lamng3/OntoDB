#include "buffer/buffer_pool_manager.h"

#include <future>
#include <sstream>

#include "common/crash_point.h"
#include "common/exception.h"
#include "recovery/log_manager.h"

namespace ontodb {

BufferPoolManager::BufferPoolManager(size_t pool_size, DiskManager* disk_manager, size_t replacer_k,
                                     LogManager* log_manager, IoStats* stats, TraceSink* trace)
    : pool_size_(pool_size)
    , disk_manager_(disk_manager)
    , replacer_k_(replacer_k)
    , log_manager_(log_manager)
    , stats_(stats)
    , trace_(trace)
    , frames_(pool_size)
    , replacer_(pool_size, replacer_k, trace)
    , scheduler_(std::make_unique<DiskScheduler>(disk_manager, trace)) {
  for (size_t i = 0; i < pool_size_; i++) {
    free_list_.push_back(static_cast<frame_id_t>(i));
  }
}

BufferPoolManager::~BufferPoolManager() = default;

auto BufferPoolManager::NewPage(page_id_t* page_id) -> Page* {
  std::lock_guard<std::mutex> guard(mu_);
  frame_id_t frame_id = INVALID_FRAME_ID;
  if (!TakeFrame(&frame_id)) {
    return nullptr;
  }
  const page_id_t id = disk_manager_->AllocatePage();
  Page& page = frames_[frame_id];
  page.Reset();
  page.SetPageId(id);
  page.IncrementPin();
  page_table_[id] = frame_id;
  replacer_.RecordAccess(frame_id);
  replacer_.SetEvictable(frame_id, false);
  *page_id = id;
  Trace("new page " + std::to_string(id) + " in frame " + std::to_string(frame_id));
  return &page;
}

auto BufferPoolManager::FetchPage(page_id_t page_id) -> Page* {
  std::lock_guard<std::mutex> guard(mu_);
  if (page_id < 0 || deleted_.count(page_id) != 0) {
    return nullptr;
  }
  if (const auto it = page_table_.find(page_id); it != page_table_.end()) {
    Page& page = frames_[it->second];
    page.IncrementPin();
    replacer_.RecordAccess(it->second);
    replacer_.SetEvictable(it->second, false);
    if (stats_ != nullptr) {
      stats_->BufferHit();
    }
    Trace("hit page " + std::to_string(page_id));
    return &page;
  }
  frame_id_t frame_id = INVALID_FRAME_ID;
  if (!TakeFrame(&frame_id)) {
    return nullptr;
  }
  Page& page = frames_[frame_id];
  page.Reset();
  page.SetPageId(page_id);
  try {
    RunIo(false, &page);
  } catch (...) {
    page.Reset();
    free_list_.push_back(frame_id);
    throw;
  }
  page.IncrementPin();
  page_table_[page_id] = frame_id;
  replacer_.RecordAccess(frame_id);
  replacer_.SetEvictable(frame_id, false);
  if (stats_ != nullptr) {
    stats_->BufferMiss();
  }
  Trace("miss page " + std::to_string(page_id) + " into frame " + std::to_string(frame_id));
  return &page;
}

auto BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty) -> bool {
  std::lock_guard<std::mutex> guard(mu_);
  const auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return false;
  }
  Page& page = frames_[it->second];
  if (page.GetPinCount() <= 0) {
    return false;
  }
  page.DecrementPin();
  if (is_dirty) {
    page.SetDirty(true);
  }
  if (page.GetPinCount() == 0) {
    replacer_.SetEvictable(it->second, true);
  }
  Trace("unpin page " + std::to_string(page_id) + (is_dirty ? " dirty" : ""));
  return true;
}

auto BufferPoolManager::FlushPage(page_id_t page_id) -> bool {
  std::lock_guard<std::mutex> guard(mu_);
  const auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return false;
  }
  Page& page = frames_[it->second];
  if (page.IsDirty()) {
    WriteBack(&page);
  }
  return true;
}

void BufferPoolManager::FlushAllPages() {
  std::lock_guard<std::mutex> guard(mu_);
  for (auto& page : frames_) {
    if (page.GetPageId() != INVALID_PAGE_ID && page.IsDirty()) {
      WriteBack(&page);
    }
  }
}

auto BufferPoolManager::DeletePage(page_id_t page_id) -> bool {
  std::lock_guard<std::mutex> guard(mu_);
  if (const auto it = page_table_.find(page_id); it != page_table_.end()) {
    Page& page = frames_[it->second];
    if (page.GetPinCount() > 0) {
      return false;
    }
    replacer_.Remove(it->second);
    free_list_.push_back(it->second);
    page.Reset();  // a deleted page's bytes are discarded, not written
    page_table_.erase(it);
  }
  disk_manager_->DeallocatePage(page_id);
  deleted_.insert(page_id);
  Trace("delete page " + std::to_string(page_id));
  return true;
}

auto BufferPoolManager::GetPinnedFrameCount() const -> size_t {
  std::lock_guard<std::mutex> guard(mu_);
  size_t pinned = 0;
  for (const auto& page : frames_) {
    if (page.GetPinCount() > 0) {
      pinned++;
    }
  }
  return pinned;
}

auto BufferPoolManager::DebugString() const -> std::string {
  std::lock_guard<std::mutex> guard(mu_);
  size_t pinned = 0;
  size_t dirty = 0;
  std::ostringstream rows;
  for (size_t i = 0; i < frames_.size(); i++) {
    const Page& page = frames_[i];
    if (page.GetPageId() == INVALID_PAGE_ID) {
      continue;
    }
    pinned += page.GetPinCount() > 0 ? 1 : 0;
    dirty += page.IsDirty() ? 1 : 0;
    rows << "frame " << i << ": page " << page.GetPageId() << ", pins " << page.GetPinCount()
         << ", " << (page.IsDirty() ? "dirty" : "clean") << ", pageLSN " << page.GetLSN() << '\n';
  }
  std::ostringstream out;
  out << "buffer pool: " << pool_size_ << " frames, lru_k " << replacer_k_ << ", "
      << page_table_.size() << " in use, " << free_list_.size() << " free, " << pinned
      << " pinned, " << dirty << " dirty, " << replacer_.Size() << " evictable\n"
      << rows.str();
  return out.str();
}

// The guards turn a null page (a full pool, or a deleted page) into an error
// at the call that asked for it. The latch is taken after FetchPage returns,
// outside mu_, so waiting for a page never blocks the whole pool.
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
  std::lock_guard<std::mutex> guard(mu_);
  if (page_id < 0 || deleted_.count(page_id) != 0) {
    return "the page was deleted or never existed";
  }
  return "every frame is pinned";
}

// A free frame first; otherwise evict, writing the victim back if dirty.
auto BufferPoolManager::TakeFrame(frame_id_t* frame_id) -> bool {
  if (!free_list_.empty()) {
    *frame_id = free_list_.front();
    free_list_.pop_front();
    return true;
  }
  frame_id_t victim = INVALID_FRAME_ID;
  if (!replacer_.Evict(&victim)) {
    return false;
  }
  Page& page = frames_[victim];
  if (page.IsDirty()) {
    try {
      WriteBack(&page);
    } catch (...) {
      // The victim stays resident and dirty; give it back to the replacer.
      replacer_.RecordAccess(victim);
      replacer_.SetEvictable(victim, true);
      throw;
    }
  }
  Trace("evict page " + std::to_string(page.GetPageId()) + " from frame " + std::to_string(victim));
  page_table_.erase(page.GetPageId());
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
