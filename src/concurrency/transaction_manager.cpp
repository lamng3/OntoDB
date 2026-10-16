#include "concurrency/transaction_manager.h"

#include <algorithm>
#include <sstream>
#include <vector>

#include "common/exception.h"

namespace ontodb {

namespace {

auto StateName(TransactionState state) -> const char* {
  switch (state) {
    case TransactionState::kGrowing:
      return "growing";
    case TransactionState::kShrinking:
      return "shrinking";
    case TransactionState::kCommitted:
      return "committed";
    case TransactionState::kAborted:
      return "aborted";
  }
  return "?";
}

auto IsolationName(IsolationLevel level) -> const char* {
  switch (level) {
    case IsolationLevel::kReadUncommitted:
      return "ru";
    case IsolationLevel::kReadCommitted:
      return "rc";
    case IsolationLevel::kRepeatableRead:
      return "rr";
    case IsolationLevel::kSerializable:
      return "ser";
  }
  return "?";
}

}  // namespace

TransactionManager::TransactionManager(LockManager* lock_manager, TripleStore* store,
                                       LogManager* log)
    : lock_manager_(lock_manager), store_(store), log_(log) {}

auto TransactionManager::Begin(IsolationLevel level) -> Transaction* {
  std::lock_guard<std::mutex> guard(mu_);
  const txn_id_t id = next_++;
  auto txn = std::make_unique<Transaction>(id, level);
  Transaction* raw = txn.get();
  txns_.emplace(id, std::move(txn));
  return raw;
}

// Strict two-phase locking: every lock is held until here.
void TransactionManager::Commit(Transaction* txn) {
  if (txn->GetState() == TransactionState::kCommitted ||
      txn->GetState() == TransactionState::kAborted) {
    throw std::logic_error("transaction " + std::to_string(txn->GetId()) + " is already finished");
  }
  txn->SetState(TransactionState::kCommitted);
  lock_manager_->UnlockAll(txn);
}

// Undo before unlocking, so no other transaction sees a change being undone.
// Without a log the write set is undone newest first (PLAN 7.2); with one,
// undo goes through the log and writes CLRs (PLAN 8.4).
void TransactionManager::Abort(Transaction* txn) {
  if (txn->GetState() == TransactionState::kCommitted) {
    throw std::logic_error("transaction " + std::to_string(txn->GetId()) + " already committed");
  }
  if (log_ != nullptr) {
    throw NotImplementedException("PLAN 8.4: TransactionManager::Abort with a log");
  }
  auto& writes = txn->GetWriteSet();
  for (auto it = writes.rbegin(); it != writes.rend(); ++it) {
    if (it->is_insert) {
      store_->Delete(it->triple);
    } else {
      store_->Insert(it->triple);
    }
  }
  writes.clear();
  txn->SetState(TransactionState::kAborted);
  lock_manager_->UnlockAll(txn);
}

auto TransactionManager::GetTransaction(txn_id_t id) -> Transaction* {
  std::lock_guard<std::mutex> guard(mu_);
  const auto it = txns_.find(id);
  return it == txns_.end() ? nullptr : it->second.get();
}

auto TransactionManager::DebugString() const -> std::string {
  std::lock_guard<std::mutex> guard(mu_);
  std::vector<txn_id_t> ids;
  for (const auto& [id, unused] : txns_) {
    ids.push_back(id);
  }
  std::sort(ids.begin(), ids.end());
  std::ostringstream out;
  for (const txn_id_t id : ids) {
    const Transaction& txn = *txns_.at(id);
    out << "txn " << id << ": " << StateName(txn.GetState()) << ", "
        << IsolationName(txn.GetIsolationLevel()) << ", "
        << const_cast<Transaction&>(txn).GetWriteSet().size() << " writes\n";
  }
  return out.str();
}

}  // namespace ontodb
