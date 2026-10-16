#include "concurrency/transaction.h"

#include <cstring>

#include "concurrency/lock_manager.h"

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

}  // namespace ontodb
