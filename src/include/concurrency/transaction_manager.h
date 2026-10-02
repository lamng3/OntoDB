#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "concurrency/lock_manager.h"
#include "concurrency/transaction.h"
#include "store/triple_store.h"

namespace ontodb {

class LogManager;

// Begin returns a growing transaction. Commit releases every lock and moves
// to kCommitted. Abort undoes the transaction, releases locks, and moves to
// kAborted. With a null log, undo walks the write set newest-first (PLAN 7.2).
// With a log, undo is logical and writes a CLR per undone triple (PLAN 8.4).
// DebugString lists id, state, and isolation, one transaction per line.
class TransactionManager {
 public:
  TransactionManager(LockManager* lock_manager, TripleStore* store, LogManager* log = nullptr);
  auto Begin(IsolationLevel level) -> Transaction*;
  void Commit(Transaction* txn);
  void Abort(Transaction* txn);
  auto GetTransaction(txn_id_t id) -> Transaction*;
  auto DebugString() const -> std::string;

 private:
  LockManager* lock_manager_;
  TripleStore* store_;
  LogManager* log_;
  std::unordered_map<txn_id_t, std::unique_ptr<Transaction>> txns_;
  txn_id_t next_{1};
};

}  // namespace ontodb
