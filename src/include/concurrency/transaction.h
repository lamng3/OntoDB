#pragma once

#include <atomic>
#include <optional>
#include <vector>

#include "common/types.h"
#include "concurrency/version_index.h"

namespace ontodb {

enum class TransactionState { kGrowing, kShrinking, kCommitted, kAborted };

// One transaction. The write set records inserted and deleted triples so
// Abort can undo them until PLAN 8.4 replaces that with log undo. A
// transaction in kCommitted or kAborted accepts no further reads or writes.
// txn ids increase by one each Begin. The younger transaction is the one
// with the larger id.
class Transaction {
 public:
  struct WriteRecord {
    bool is_insert{true};
    Triple triple;
  };

  Transaction(txn_id_t id, IsolationLevel isolation);

  auto GetId() const -> txn_id_t { return id_; }
  auto GetIsolationLevel() const -> IsolationLevel { return isolation_; }
  // Atomic: the deadlock detector marks a waiting transaction aborted from its
  // own thread.
  auto GetState() const -> TransactionState { return state_.load(); }
  void SetState(TransactionState state);
  auto GetWriteSet() -> std::vector<WriteRecord>&;
  // The LSN of this transaction's newest log record (PLAN 8.4), the prev_lsn
  // of the next one it writes.
  auto GetLastLsn() const -> lsn_t { return last_lsn_; }
  void SetLastLsn(lsn_t lsn) { last_lsn_ = lsn; }

  // When set, this transaction reads the database as of that commit, without locks.
  auto GetSnapshot() const -> std::optional<commit_ts_t> { return snapshot_; }
  void SetSnapshot(commit_ts_t at) { snapshot_ = at; }
  // First committer wins: a write fails if another transaction committed a change to the same
  // triple after this one's snapshot. A statement outside \\begin read nothing earlier that the
  // write could contradict, so it does not check.
  auto ChecksConflicts() const -> bool { return checks_conflicts_; }
  void SetChecksConflicts(bool checks) { checks_conflicts_ = checks; }

 private:
  txn_id_t id_;
  IsolationLevel isolation_;
  std::atomic<TransactionState> state_{TransactionState::kGrowing};
  std::vector<WriteRecord> write_set_;
  lsn_t last_lsn_{INVALID_LSN};
  std::optional<commit_ts_t> snapshot_;
  bool checks_conflicts_{false};
};

}  // namespace ontodb
