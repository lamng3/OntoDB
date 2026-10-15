#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common/io_stats.h"
#include "common/types.h"
#include "dictionary/mem_dictionary.h"
#include "recovery/log_manager.h"
#include "store/mem_store.h"
#include "store/recording_store.h"

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
// and `wal`. Each statement is one transaction: begin, any new dictionary
// terms, one update record per changed triple, commit. The commit is forced
// before the statement returns. Opening loads the snapshot and replays the
// committed transactions logged after it; an uncommitted transaction leaves
// no trace. A statement that fails partway is undone in memory and logs
// nothing it needs.
//
// Updates are logical (page_id is INVALID_PAGE_ID): the memory store has no
// pages. The disk store's page-oriented recovery is PLAN 8.3 to 8.7.
class Durability {
 public:
  Durability(std::filesystem::path dir, MemDictionary* dictionary, MemStore* store, IoStats* stats);
  ~Durability();

  Durability(const Durability&) = delete;
  auto operator=(const Durability&) -> Durability& = delete;

  // Call once, on an empty dictionary and store.
  auto Open(RecoveryMode mode) -> OpenReport;

  // Runs `body` as one transaction. `body` makes its changes through the
  // store it is given.
  void Run(const std::function<void(TripleStore&)>& body);

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
  // Open, in steps. Each returns the LSN it reached.
  auto LoadSnapshotIfAny(OpenReport* report) -> lsn_t;
  auto ReplayLog(const LogScan& scan, lsn_t snapshot_lsn, OpenReport* report) -> lsn_t;
  // Run: begin, new terms, one update per change, commit, forced.
  void LogTransaction(const std::vector<RecordingStore::Change>& changes);
  // Verify, one check per report line.
  auto CheckSnapshot(MemDictionary* terms, MemStore* store,
                     lsn_t* snapshot_lsn) const -> CheckResult;
  static auto CheckLog(const LogScan& scan) -> CheckResult;
  auto CheckReplay(const LogScan& scan, lsn_t snapshot_lsn, MemDictionary* terms,
                   MemStore* store) const -> CheckResult;
  auto CheckInvariants() const -> CheckResult;

  std::filesystem::path dir_;
  MemDictionary* dictionary_;
  MemStore* store_;
  IoStats* stats_;

  std::mutex mu_;
  std::unique_ptr<LogManager> log_;
  size_t logged_terms_{0};
  txn_id_t next_txn_{1};
};

}  // namespace ontodb
