#pragma once

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "buffer/lru_k_replacer.h"
#include "common/io_stats.h"
#include "common/trace.h"
#include "storage/disk/disk_manager.h"
#include "storage/disk/disk_scheduler.h"
#include "storage/page/page.h"
#include "storage/page/page_guard.h"

namespace ontodb {

class LogManager;

// Thread-safe cache of pages in front of a DiskManager.
//
// NewPage allocates a disk page, pins it, and returns the zeroed frame.
// FetchPage pins an existing page, reading it from disk on a miss.
// UnpinPage drops one pin and, if is_dirty is true, marks the frame dirty.
// FlushPage writes a dirty frame only. If log_manager is non-null, the log
// is flushed up to the page's pageLSN before the frame is written. Call
// CrashPoint::Reach("BufferPoolManager::FlushPage::before_disk_write") after
// that log flush and before the disk write.
// DeletePage succeeds only when the page is not pinned. It must not be
// fetched again.
// GetPinnedFrameCount is the number of frames whose pin count is positive.
// It is safe to call concurrently and is what the harness checks after every
// query and every destroyed iterator.
//
// FetchPageRead / FetchPageWrite / NewPageGuarded return guards (PLAN 1.5).
// A read guard takes a shared latch on the frame; a write guard takes an
// exclusive latch. Latches are held until the guard is dropped.
//
// stats: BufferHit on a fetch served from memory, BufferMiss when a frame is
// read from disk. Trace component "bpm".
//
// One latch per shard covers that shard's page table, free list, pins, and dirty bits, and is
// held across the disk I/O of a miss or a write-back there. A pool has `shards` shards (default
// 1, the behavior every test relies on); a page belongs to shard page_id mod shards, and each
// shard owns an equal share of the frames and its own LRU-K replacer, so threads that touch
// pages of different shards do not wait on one another. With more than one shard NewPage
// allocates the page id before it looks for a frame, so a pool that cannot give a frame leaves
// that id unused.
//
// With several shards a hit does not take the shard latch. The page index is read without it, the
// pin is a compare-and-swap on the frame's pin count, and the hit is only marked on the frame;
// the replacer learns of marked frames when the shard must evict. Eviction first claims a frame
// whose pin count is zero by swapping it to -1, which keeps every later pinner out, so a frame is
// never evicted under a reader. A pin that raced with an eviction finds the frame holding another
// page and lets go. Eviction order is then the LRU-K order of the accesses the replacer has seen
// (every miss and every marked frame once), not of every access. All I/O goes
// through a DiskScheduler. NewPage and FetchPage return nullptr when every
// frame is pinned, or every unpinned frame is dirty with a pageLSN an open
// page group still holds back in the log (see PageGroup below). The destructor does not flush:
// durability is the log's job, and a clean shutdown calls FlushAllPages first.
class BufferPoolManager {
 public:
  BufferPoolManager(size_t pool_size, DiskManager* disk_manager, size_t replacer_k,
                    LogManager* log_manager = nullptr, IoStats* stats = nullptr,
                    TraceSink* trace = nullptr, size_t shards = 1);
  ~BufferPoolManager();

  BufferPoolManager(const BufferPoolManager&) = delete;
  auto operator=(const BufferPoolManager&) -> BufferPoolManager& = delete;

  auto NewPage(page_id_t* page_id) -> Page*;
  auto FetchPage(page_id_t page_id) -> Page*;
  auto UnpinPage(page_id_t page_id, bool is_dirty) -> bool;
  auto FlushPage(page_id_t page_id) -> bool;
  void FlushAllPages();
  auto DeletePage(page_id_t page_id) -> bool;
  auto GetPinnedFrameCount() const -> size_t;
  auto GetPoolSize() const -> size_t { return pool_size_; }
  // With a log manager installed: appends a page record holding the page's
  // bytes after its pageLSN and sets the pageLSN to that record's LSN
  // (PLAN 8.3). A write guard calls it, latch still held, when it releases a
  // page it changed, so every change through a guard is logged before the
  // page can be written. Without a log manager it does nothing.
  void LogPageWrite(Page* page);
  // Forward to the log manager's BeginGroup / EndGroup; no-ops without one.
  auto BeginGroup() -> lsn_t;
  void EndGroup(lsn_t token);
  auto DebugString() const -> std::string;

  auto FetchPageRead(page_id_t page_id) -> ReadPageGuard;
  auto FetchPageWrite(page_id_t page_id) -> WritePageGuard;
  auto NewPageGuarded(page_id_t* page_id) -> WritePageGuard;

 private:
  // The frames, page table, free list, and replacer of one shard, under one latch.
  struct Shard {
    Shard(size_t frame_count, size_t k, TraceSink* trace);
    mutable std::mutex mu;
    std::vector<Page> frames;
    std::unordered_map<page_id_t, frame_id_t> page_table;
    std::deque<frame_id_t> free_list;
    std::unordered_set<page_id_t> deleted;
    LRUKReplacer replacer;
    // The resident pages again, as an open-addressing table that is written under mu and read
    // without it: entry = ((page id + 1) << 24) | frame, 0 empty, 1 deleted. Used only when the
    // pool has several shards.
    std::vector<std::atomic<uint64_t>> index;
    size_t deleted_entries{0};
    void IndexInsert(page_id_t page_id, frame_id_t frame);
    void IndexErase(page_id_t page_id);
    auto IndexFind(page_id_t page_id) const -> frame_id_t;  // -1 when absent
  };

  auto ShardOf(page_id_t page_id) const -> Shard&;
  auto TakeFrame(Shard& shard, frame_id_t* frame_id) -> bool;  // requires shard.mu
  void WriteBack(Page* page);                                  // requires the page's shard latch
  void RunIo(bool is_write, Page* page);                       // requires the page's shard latch
  void Trace(const std::string& message) const;
  auto NullReason(page_id_t page_id) const -> std::string;

  size_t pool_size_;
  DiskManager* disk_manager_;
  size_t replacer_k_;
  LogManager* log_manager_;
  IoStats* stats_;
  TraceSink* trace_;

  std::vector<std::unique_ptr<Shard>> shards_;
  std::unique_ptr<DiskScheduler> scheduler_;
};

// One multi-page change, logged atomically (a mini-transaction). Every page
// record appended while it lives reaches the log file together with the
// others, or none does; until then none of those pages is written. A B+ tree
// Insert or Remove opens one before its first latch and closes it after its
// last, so a crash never leaves half a split or half a merge for redo.
class PageGroup {
 public:
  explicit PageGroup(BufferPoolManager* bpm) : bpm_(bpm), token_(bpm->BeginGroup()) {}
  ~PageGroup() { bpm_->EndGroup(token_); }
  PageGroup(const PageGroup&) = delete;
  auto operator=(const PageGroup&) -> PageGroup& = delete;

 private:
  BufferPoolManager* bpm_;
  lsn_t token_;
};

}  // namespace ontodb
