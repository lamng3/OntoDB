#include "concurrency/transaction_manager.h"

#include <algorithm>
#include <sstream>
#include <vector>

#include "common/exception.h"
#include "recovery/log_manager.h"

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
  if (log_ != nullptr) {
    auto record = LogRecord::Begin(id);
    raw->SetLastLsn(log_->Append(&record));
  }
  return raw;
}

auto TransactionManager::LogUpdate(Transaction* txn, bool is_insert,
                                   const Triple& triple) -> lsn_t {
  if (log_ == nullptr) {
    return INVALID_LSN;
  }
  auto record = LogRecord::Update(txn->GetId(), INVALID_PAGE_ID, is_insert, triple);
  record.SetPrevLsn(txn->GetLastLsn());
  const lsn_t lsn = log_->Append(&record);
  txn->SetLastLsn(lsn);
  return lsn;
}

auto TransactionManager::ActiveTransactions() const -> std::vector<txn_id_t> {
  std::lock_guard<std::mutex> guard(mu_);
  std::vector<txn_id_t> active;
  for (const auto& [id, txn] : txns_) {
    if (txn->GetState() == TransactionState::kGrowing ||
        txn->GetState() == TransactionState::kShrinking) {
      active.push_back(id);
    }
  }
  std::sort(active.begin(), active.end());
  return active;
}

// Strict two-phase locking: every lock is held until here.
void TransactionManager::Commit(Transaction* txn) {
  if (txn->GetState() == TransactionState::kCommitted ||
      txn->GetState() == TransactionState::kAborted) {
    throw std::logic_error("transaction " + std::to_string(txn->GetId()) + " is already finished");
  }
  if (log_ != nullptr) {
    auto record = LogRecord::Commit(txn->GetId());
    record.SetPrevLsn(txn->GetLastLsn());
    const lsn_t lsn = log_->Append(&record);
    txn->SetLastLsn(lsn);
    log_->Flush(lsn, true);  // durable before the commit is acknowledged
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
    UndoByLog(txn);
    txn->GetWriteSet().clear();
    txn->SetState(TransactionState::kAborted);
    lock_manager_->UnlockAll(txn);
    return;
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

void TransactionManager::UndoByLog(Transaction* txn) {
  std::vector<LogRecord> chain;  // this transaction's updates and CLRs, oldest first
  for (auto& record : log_->ReadAll()) {
    if (record.GetTxnId() == txn->GetId() &&
        (record.GetType() == LogRecordType::kUpdate || record.GetType() == LogRecordType::kClr)) {
      chain.push_back(std::move(record));
    }
  }
  UndoChain(txn->GetId(), chain, store_, log_, txn->GetLastLsn());
  auto end = LogRecord::Abort(txn->GetId());
  end.SetPrevLsn(txn->GetLastLsn());
  log_->Flush(log_->Append(&end), true);
}

auto TransactionManager::UndoChain(txn_id_t txn, const std::vector<LogRecord>& chain,
                                   TripleStore* store, LogManager* log, lsn_t prev) -> size_t {
  // Each update's predecessor among this transaction's updates.
  std::vector<size_t> updates;
  for (size_t i = 0; i < chain.size(); i++) {
    if (chain[i].GetType() == LogRecordType::kUpdate) {
      updates.push_back(i);
    }
  }
  auto previous_update = [&](lsn_t below) -> lsn_t {
    lsn_t found = INVALID_LSN;
    for (const size_t i : updates) {
      if (chain[i].GetLsn() < below) {
        found = chain[i].GetLsn();
      }
    }
    return found;
  };
  // Start at the newest record: a CLR says where undo left off.
  lsn_t next = INVALID_LSN;
  if (!chain.empty()) {
    next = chain.back().GetType() == LogRecordType::kClr ? chain.back().GetUndoNext()
                                                         : chain.back().GetLsn();
  }
  size_t undone = 0;
  while (next != INVALID_LSN) {
    const auto it = std::find_if(chain.begin(), chain.end(),
                                 [&](const LogRecord& r) { return r.GetLsn() == next; });
    if (it == chain.end()) {
      break;
    }
    const Triple triple = it->GetTriple();
    const bool reinsert = !it->IsInsert();
    if (reinsert) {
      store->Insert(triple);
    } else {
      store->Delete(triple);
    }
    const lsn_t undo_next = previous_update(it->GetLsn());
    auto clr = LogRecord::Clr(txn, INVALID_PAGE_ID, reinsert, triple, undo_next);
    clr.SetPrevLsn(prev);
    prev = log->Append(&clr);
    undone++;
    next = undo_next;
  }
  return undone;
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
