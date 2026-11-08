#pragma once

#include <atomic>
#include <mutex>
#include <set>
#include <string>

#include "common/io_stats.h"
#include "common/trace.h"
#include "common/types.h"

namespace ontodb {

// Owns the database file. Page ids allocated here are stable across restarts.
// ReadPage and WritePage move exactly PAGE_SIZE bytes, including the pageLSN
// in the first 8 bytes. A page id that was never written reads back as zeros.
// ShutDown flushes and closes the file. The object is not copyable.
//
// Report every read with stats->PageRead and every write with stats->PageWrite
// when stats is non-null. Trace component "disk".
//
// The file's length is the high-water mark: AllocatePage grows the file by one
// zero page when nothing freed is waiting, so a restart sees every page that
// was still allocated, and page 0 is an ordinary page (the catalog uses it).
// DeallocatePage hands the id out again. A freed page at the end of the file
// is cut off, and so is each freed page that then becomes the end, so deleting
// the tail shrinks the file. A hole in the middle stays allocated until this
// process reuses it: the free list is not written down, because a crash must
// not hand out a page the log may still redo. WritePage does not fsync;
// ShutDown does. Writing a page past the end grows the file to cover it, and
// reading one returns zeros, so a never-written page reads as zeros whether or
// not it was allocated. A negative page id, or any call after ShutDown, throws
// StorageException.
class DiskManager {
 public:
  explicit DiskManager(std::string db_file, IoStats* stats = nullptr, TraceSink* trace = nullptr);
  ~DiskManager();

  DiskManager(const DiskManager&) = delete;
  auto operator=(const DiskManager&) -> DiskManager& = delete;

  void ShutDown();
  void WritePage(page_id_t page_id, const char* page_data);
  void ReadPage(page_id_t page_id, char* page_data);
  auto AllocatePage() -> page_id_t;
  void DeallocatePage(page_id_t page_id);

  auto GetNumWrites() const -> int;
  auto GetNumReads() const -> int;

 private:
  void Trace(const char* op, page_id_t page_id);
  void CheckUsable(page_id_t page_id) const;

  std::string db_file_;
  IoStats* stats_;
  TraceSink* trace_;
  int fd_{-1};
  std::mutex alloc_mu_;
  std::set<page_id_t> free_;  // ids DeallocatePage handed back, lowest first
  std::atomic<page_id_t> num_pages_{0};
  std::atomic<int> num_reads_{0};
  std::atomic<int> num_writes_{0};
};

}  // namespace ontodb
