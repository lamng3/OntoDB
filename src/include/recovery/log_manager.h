#pragma once

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

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
//
// Page groups (a mini-transaction): BeginGroup returns a token, and until
// EndGroup(token) nothing at or after the token's LSN is flushed, so the
// records of one multi-page change (a B+ tree split or merge) reach the file
// all together or not at all. FlushableLsn is the newest LSN a flush may write
// now. A page whose pageLSN is newer must not be written yet; the buffer pool
// chooses another victim. Flush(lsn) for a held-back lsn waits for EndGroup,
// so the thread that owns the group must not ask for it.
//
// Opening an existing log continues its LSNs after the last valid record.
// Without a running flush thread, Flush writes on the caller's thread.
// LSNs start at 1. The destructor flushes; a crash (`std::_Exit`) does not.
class LogManager {
 public:
  explicit LogManager(std::string log_path, IoStats* stats = nullptr);
  ~LogManager();

  LogManager(const LogManager&) = delete;
  auto operator=(const LogManager&) -> LogManager& = delete;

  void StartFlushThread();
  void StopFlushThread();
  auto Append(LogRecord* record) -> lsn_t;
  void Flush(lsn_t lsn, bool force);
  // Makes durable everything that may be flushed now, and waits for it.
  void FlushAvailable();
  auto BeginGroup() -> lsn_t;
  void EndGroup(lsn_t token);
  auto FlushableLsn() const -> lsn_t;
  auto GetPersistentLsn() const -> lsn_t;
  auto GetFlushCount() const -> uint64_t;
  auto DebugTail(size_t n) const -> std::string;

  // The next Append gets at least `lsn`. Used after a snapshot so LSNs never repeat.
  void AdvanceNextLsn(lsn_t lsn);
  auto NextLsn() const -> lsn_t;
  auto GetPath() const -> const std::string& { return log_path_; }
  // Flushes everything appended so far and returns every valid record in the
  // file, oldest first. Undo uses it to find a transaction's records.
  auto ReadAll() -> std::vector<LogRecord>;

 private:
  void FlushThreadLoop();
  // Writes the buffer and fsyncs. Called with `lock` held; drops it during I/O.
  void WriteBuffered(std::unique_lock<std::mutex>& lock);
  // The newest buffered LSN that no open group holds back. Needs mu_.
  auto Limit() const -> lsn_t;

  std::string log_path_;
  IoStats* stats_;
  int fd_{-1};

  mutable std::mutex mu_;
  std::condition_variable work_cv_;
  std::condition_variable durable_cv_;
  std::vector<char> buffer_;
  std::vector<std::pair<lsn_t, size_t>> ends_;  // each buffered record's LSN and end offset
  std::multiset<lsn_t> open_groups_;            // first LSN of each open group
  bool held_back_{false};                       // the last write stopped at a group
  lsn_t next_lsn_{1};
  lsn_t buffered_lsn_{0};
  lsn_t persistent_lsn_{0};
  uint64_t flush_count_{0};
  bool writing_{false};
  bool flush_requested_{false};
  bool running_{false};
  bool stop_{false};
  std::string failure_;  // set when the flush thread's write fails
  std::thread thread_;
};

// The valid prefix of a log file. Scanning stops at the first record that is
// short (a torn write), fails its checksum, or has an impossible header.
// `valid_bytes` is where that record starts; `stop` says why, empty when the
// whole file is valid. `torn` is true when the bad record is a short final
// write, false when its bytes are present but wrong.
struct LogScan {
  std::vector<LogRecord> records;
  uint64_t valid_bytes{0};
  uint64_t file_bytes{0};
  std::string stop;
  bool torn{false};

  auto Clean() const -> bool { return stop.empty(); }
};

auto ScanLog(const std::filesystem::path& path) -> LogScan;

}  // namespace ontodb
