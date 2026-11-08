#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common/io_stats.h"
#include "common/types.h"
#include "concurrency/version_index.h"
#include "dictionary/mem_dictionary.h"
#include "recovery/log_manager.h"
#include "store/mem_store.h"

namespace ontodb {

// One line of \verify's report.
struct CheckResult {
  bool ok;
  std::string line;
};

// What to do with a log record that is torn or corrupt (PLAN 9.5). The names
// follow RocksDB's WAL recovery modes.
//   kPointInTime          keep every transaction before the bad record, drop
//                         the bad record and everything after it, and report
//                         it. This is also what a torn final write after a
//                         crash needs.
//   kAbsoluteConsistency  refuse to open.
enum class RecoveryMode { kPointInTime, kAbsoluteConsistency };

struct OpenReport {
  std::filesystem::path dir;
  bool had_snapshot{false};
  lsn_t snapshot_lsn{0};
  size_t snapshot_triples{0};
  size_t log_records{0};
  size_t committed{0};
  size_t uncommitted{0};
  size_t triples{0};
  std::string stop;  // empty when the log was clean
  uint64_t discarded_bytes{0};

  auto ToString() const -> std::string;
};

// Durability for the memory database (PLAN 9). The directory holds `snapshot`
// and `wal`. A transaction reaches the log whole, when it commits: begin, any
// new dictionary terms, one update record per changed triple, commit, forced
// before the commit returns. So a transaction still open has nothing on disk,
// and undoing one, or a crash during one, needs nothing from the log. Opening
// loads the snapshot and replays the committed transactions logged after it,
// in order, each as the next commit in the version index, which is how every
// commit keeps its timestamp. A snapshot holds the index's whole history, so a
// checkpoint loses no version. Tags are logged and forced like commits.
//
// One process writes a directory at a time, under an exclusive lock on its `lock` file, which the
// system releases if the process dies. A read-only open takes no lock and changes nothing: it sees
// the commits that were whole when it opened, however far a writer has got, so it never waits for a
// writer and a writer never waits for it. A checkpoint can only make what it read older, never
// inconsistent: replay starts past the snapshot's LSN, and the log is a prefix of what was
// committed.
//
// Updates are logical (page_id is INVALID_PAGE_ID): the memory store has no
// pages. The disk store's page-oriented recovery is PLAN 8.3 to 8.7.
class Durability {
 public:
  Durability(std::filesystem::path dir, MemDictionary* dictionary, MemStore* store,
             VersionIndex* versions, IoStats* stats);
  ~Durability();

  Durability(const Durability&) = delete;
  auto operator=(const Durability&) -> Durability& = delete;

  // Call once, on an empty dictionary and store.
  auto Open(RecoveryMode mode, bool read_only = false) -> OpenReport;

  // Logs one transaction's changes and forces them. The caller holds off other commits, so the
  // log's order is the order of the timestamps.
  void LogCommit(const std::vector<VersionChange>& changes);
  void LogTag(const std::string& name, commit_ts_t at);

  // Writes a snapshot of the current state and starts an empty log.
  auto Checkpoint() -> std::string;

  // Rebuilds the state from the files on disk, compares it with the live
  // state, and checks checksums and invariants. Changes nothing.
  auto Verify() -> std::string;

  auto LogTail(size_t n) const -> std::string;
  auto SnapshotPath() const -> std::filesystem::path { return dir_ / "snapshot"; }
  auto WalPath() const -> std::filesystem::path { return dir_ / "wal"; }

 private:
  void StartLog(lsn_t next_lsn);
  void TakeWriterLock();
  // Open, in steps. Each returns the LSN it reached.
  auto LoadSnapshotIfAny(OpenReport* report) -> lsn_t;
  auto ReplayLog(const LogScan& scan, lsn_t snapshot_lsn, OpenReport* report) -> lsn_t;
  // Verify, one check per report line.
  auto CheckSnapshot(MemDictionary* terms, MemStore* store, VersionIndex* versions,
                     lsn_t* snapshot_lsn) const -> CheckResult;
  static auto CheckLog(const LogScan& scan) -> CheckResult;
  auto CheckReplay(const LogScan& scan, lsn_t snapshot_lsn, MemDictionary* terms, MemStore* store,
                   VersionIndex* versions) const -> CheckResult;
  auto CheckInvariants() const -> CheckResult;

  std::filesystem::path dir_;
  MemDictionary* dictionary_;
  MemStore* store_;
  VersionIndex* versions_;
  IoStats* stats_;

  std::mutex mu_;
  std::unique_ptr<LogManager> log_;
  int lock_fd_{-1};
  size_t logged_terms_{0};
  txn_id_t next_txn_{1};
};

}  // namespace ontodb
