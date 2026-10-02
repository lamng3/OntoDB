#pragma once

#include <string>

#include "common/io_stats.h"
#include "recovery/log_record.h"

namespace ontodb {

// In-memory log buffer plus a background flush thread. Append assigns an LSN
// and returns it. Flush(lsn) blocks until every record up to `lsn` is durable.
// A commit calls Flush with force true. Group commit: commits that arrive
// while a flush is already running share that flush, so GetFlushCount grows
// slower than the number of commits under concurrency. StartFlushThread
// starts the thread; StopFlushThread flushes what remains and joins it.
// Each physical flush calls stats->LogFlush() when stats is non-null.
// DebugTail returns the last n records, decoded, oldest first.
class LogManager {
 public:
  explicit LogManager(std::string log_path, IoStats* stats = nullptr);
  ~LogManager();

  void StartFlushThread();
  void StopFlushThread();
  auto Append(LogRecord* record) -> lsn_t;
  void Flush(lsn_t lsn, bool force);
  auto GetPersistentLsn() const -> lsn_t;
  auto GetFlushCount() const -> uint64_t;
  auto DebugTail(size_t n) const -> std::string;

 private:
  std::string log_path_;
  IoStats* stats_;
};

}  // namespace ontodb
