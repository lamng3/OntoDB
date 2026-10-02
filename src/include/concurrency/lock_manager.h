#pragma once

#include <string>
#include <utility>
#include <vector>

#include "common/session_state.h"
#include "concurrency/transaction.h"
#include "storage/index/triple_key.h"

namespace ontodb {

enum class LockMode { kIs, kIx, kS, kX, kSix };

enum class ResourceType { kDatabase, kIndex, kKey };

// A lockable resource. Database has an empty name. Index uses `name`
// ("spo", "pos", "osp"). Key uses `name` as the index plus `key`.
struct LockResource {
  ResourceType type{ResourceType::kDatabase};
  std::string name;
  TripleKey key{};

  auto operator==(const LockResource& other) const -> bool;
};

// Hierarchical locks. Grant order on one resource is FIFO. Lock blocks until
// the request is granted or the transaction is aborted. Before blocking, if
// `session` is non-null, call session->SetWaiting with a reason of the form
// "X lock held by txn 5", and ClearWaiting when the wait ends.
//
// Compatibility: IS with IS, IX, S, SIX. IX with IS, IX. S with IS, S.
// SIX with IS. X with nothing. Same-transaction re-entry of a stronger or
// equal mode is an upgrade and stays in the queue's place only if it does
// not jump ahead of a waiter.
//
// StartDeadlockDetection runs a background thread that aborts the youngest
// transaction in each waits-for cycle by throwing TransactionAbortException
// out of its Lock call. UnlockAll drops every lock the transaction holds.
class LockManager {
 public:
  LockManager() = default;
  ~LockManager();

  auto Lock(Transaction* txn, const LockResource& resource, LockMode mode, SessionState* session)
      -> bool;
  auto Unlock(Transaction* txn, const LockResource& resource) -> bool;
  void UnlockAll(Transaction* txn);
  void StartDeadlockDetection();
  void StopDeadlockDetection();
  auto GetLocks(txn_id_t id) const -> std::vector<std::pair<LockResource, LockMode>>;
  auto DebugString() const -> std::string;
};

}  // namespace ontodb
