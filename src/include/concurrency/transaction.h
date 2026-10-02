#pragma once

#include <vector>

#include "common/types.h"

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
  auto GetState() const -> TransactionState { return state_; }
  void SetState(TransactionState state);
  auto GetWriteSet() -> std::vector<WriteRecord>&;

 private:
  txn_id_t id_;
  IsolationLevel isolation_;
  TransactionState state_{TransactionState::kGrowing};
  std::vector<WriteRecord> write_set_;
};

}  // namespace ontodb
