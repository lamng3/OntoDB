#pragma once

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
  std::string db_file_;
  IoStats* stats_;
  TraceSink* trace_;
};

}  // namespace ontodb
