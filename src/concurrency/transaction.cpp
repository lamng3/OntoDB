#include "concurrency/transaction.h"

#include <cstring>

#include "common/exception.h"
#include "concurrency/lock_manager.h"
#include "concurrency/transaction_manager.h"

namespace ontodb {

Transaction::Transaction(txn_id_t id, IsolationLevel isolation) : id_(id), isolation_(isolation) {}

void Transaction::SetState(TransactionState state) {
  state_ = state;
}

auto Transaction::GetWriteSet() -> std::vector<WriteRecord>& {
  return write_set_;
}

auto LockResource::operator==(const LockResource& other) const -> bool {
  if (type != other.type || name != other.name) {
    return false;
  }
  if (type != ResourceType::kKey) {
    return true;
  }
  return std::memcmp(key.Data(), other.key.Data(), TripleKey::SIZE) == 0;
}

LockManager::~LockManager() = default;

auto LockManager::Lock(Transaction*, const LockResource&, LockMode, SessionState*) -> bool {
  throw NotImplementedException("PLAN 7.3: LockManager::Lock");
}
auto LockManager::Unlock(Transaction*, const LockResource&) -> bool {
  throw NotImplementedException("PLAN 7.3: LockManager::Unlock");
}
void LockManager::UnlockAll(Transaction*) {
  throw NotImplementedException("PLAN 7.3: LockManager::UnlockAll");
}
void LockManager::StartDeadlockDetection() {
  throw NotImplementedException("PLAN 7.5: LockManager::StartDeadlockDetection");
}
void LockManager::StopDeadlockDetection() {
  throw NotImplementedException("PLAN 7.5: LockManager::StopDeadlockDetection");
}
auto LockManager::GetLocks(txn_id_t) const -> std::vector<std::pair<LockResource, LockMode>> {
  throw NotImplementedException("PLAN 7.3: LockManager::GetLocks");
}
auto LockManager::DebugString() const -> std::string {
  throw NotImplementedException("PLAN 7.3: LockManager::DebugString");
}

TransactionManager::TransactionManager(LockManager* lock_manager, TripleStore* store,
                                       LogManager* log)
    : lock_manager_(lock_manager), store_(store), log_(log) {}

auto TransactionManager::Begin(IsolationLevel) -> Transaction* {
  throw NotImplementedException("PLAN 7.2: TransactionManager::Begin");
}
void TransactionManager::Commit(Transaction*) {
  throw NotImplementedException("PLAN 7.2: TransactionManager::Commit");
}
void TransactionManager::Abort(Transaction*) {
  throw NotImplementedException("PLAN 7.2: TransactionManager::Abort");
}
auto TransactionManager::GetTransaction(txn_id_t) -> Transaction* {
  throw NotImplementedException("PLAN 7.2: TransactionManager::GetTransaction");
}
auto TransactionManager::DebugString() const -> std::string {
  throw NotImplementedException("PLAN 7.2: TransactionManager::DebugString");
}

}  // namespace ontodb
