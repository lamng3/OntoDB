#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "concurrency/lock_manager.h"
#include "concurrency/transaction.h"
#include "recovery/log_record.h"
#include "store/triple_store.h"

namespace ontodb {

class LogManager;

// Begin returns a growing transaction. Commit releases every lock and moves
// to kCommitted. Abort undoes the transaction, releases locks, and moves to
// kAborted. With a null log, undo walks the write set newest-first (PLAN 7.2).
// With a log, undo is logical and writes a CLR per undone triple (PLAN 8.4).
// DebugString lists id, state, and isolation, one transaction per line.
//
// With a log (PLAN 8.4): Begin appends a begin record; LogUpdate appends an
// update record for a change about to be made, chained by prev_lsn; Commit
// appends a commit record and forces it before returning. Abort reads the log
// back, rebuilds this transaction's chain from its records, and undoes its
// updates newest first through the store, appending a CLR per undone triple
// (undo_next: the next update still to undo); a CLR already in the chain
// means the updates after its undo_next are undone, so a second abort, or
// recovery after a crash during abort, resumes there. It ends with an abort
// record. Reading the whole log is the simple way to find a chain; an engine
// with LSNs as file offsets would read it backwards record by record.
class TransactionManager {
 public:
  TransactionManager(LockManager* lock_manager, TripleStore* store, LogManager* log = nullptr);
  auto Begin(IsolationLevel level) -> Transaction*;
  void Commit(Transaction* txn);
  void Abort(Transaction* txn);
  auto GetTransaction(txn_id_t id) -> Transaction*;
  // Logs a change before it is applied; returns its LSN, or INVALID_LSN
  // without a log. Call it only for a change that will happen.
  auto LogUpdate(Transaction* txn, bool is_insert, const Triple& triple) -> lsn_t;
  auto ActiveTransactions() const -> std::vector<txn_id_t>;
  auto HasLog() const -> bool { return log_ != nullptr; }
  auto DebugString() const -> std::string;

  // Undoes `txn`'s updates in `chain` (its update and CLR records, oldest
  // first) newest first through `store`, appending a CLR for each to `log`
  // with prev_lsn chained from `prev`. Shared with recovery. Returns how many
  // updates it undid.
  static auto UndoChain(txn_id_t txn, const std::vector<LogRecord>& chain, TripleStore* store,
                        LogManager* log, lsn_t prev) -> size_t;

 private:
  void UndoByLog(Transaction* txn);

  LockManager* lock_manager_;
  TripleStore* store_;
  LogManager* log_;
  mutable std::mutex mu_;  // guards txns_ and next_; transactions run on many threads
  std::unordered_map<txn_id_t, std::unique_ptr<Transaction>> txns_;
  txn_id_t next_{1};
};

}  // namespace ontodb
