#pragma once

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
// One latch (mu_) covers the page table, the free list, pins, and dirty bits,
// and is held across the disk I/O of a miss or a write-back. All I/O goes
// through a DiskScheduler. NewPage and FetchPage return nullptr when every
// frame is pinned. The destructor does not flush: durability is the log's job,
// and a clean shutdown calls FlushAllPages first.
class BufferPoolManager {
 public:
  BufferPoolManager(size_t pool_size, DiskManager* disk_manager, size_t replacer_k,
                    LogManager* log_manager = nullptr, IoStats* stats = nullptr,
                    TraceSink* trace = nullptr);
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
  auto DebugString() const -> std::string;

  auto FetchPageRead(page_id_t page_id) -> ReadPageGuard;
  auto FetchPageWrite(page_id_t page_id) -> WritePageGuard;
  auto NewPageGuarded(page_id_t* page_id) -> WritePageGuard;

 private:
  auto TakeFrame(frame_id_t* frame_id) -> bool;  // requires mu_
  void WriteBack(Page* page);                    // requires mu_
  void RunIo(bool is_write, Page* page);         // requires mu_
  void Trace(const std::string& message) const;
  auto NullReason(page_id_t page_id) const -> std::string;

  size_t pool_size_;
  DiskManager* disk_manager_;
  size_t replacer_k_;
  LogManager* log_manager_;
  IoStats* stats_;
  TraceSink* trace_;

  mutable std::mutex mu_;
  std::vector<Page> frames_;
  std::unordered_map<page_id_t, frame_id_t> page_table_;
  std::deque<frame_id_t> free_list_;
  std::unordered_set<page_id_t> deleted_;
  LRUKReplacer replacer_;
  std::unique_ptr<DiskScheduler> scheduler_;
};

}  // namespace ontodb
