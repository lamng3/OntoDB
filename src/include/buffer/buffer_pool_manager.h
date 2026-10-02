#pragma once

#include <string>

#include "common/io_stats.h"
#include "common/trace.h"
#include "storage/disk/disk_manager.h"
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
  auto DebugString() const -> std::string;

  auto FetchPageRead(page_id_t page_id) -> ReadPageGuard;
  auto FetchPageWrite(page_id_t page_id) -> WritePageGuard;
  auto NewPageGuarded(page_id_t* page_id) -> WritePageGuard;

 private:
  size_t pool_size_;
  DiskManager* disk_manager_;
  size_t replacer_k_;
  LogManager* log_manager_;
  IoStats* stats_;
  TraceSink* trace_;
};

}  // namespace ontodb
