#pragma once

#include <atomic>
#include <condition_variable>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <thread>
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
//
// Each resource has a FIFO queue of requests. A request is granted when it is
// compatible with every lock other transactions hold on the resource and no
// request ahead of it is still waiting. An upgrade (S then X, say) keeps the
// old lock while it waits, queued behind any waiters, and replaces it when
// granted. A transaction already holding a covering mode is granted at once.
// The detector runs every 20 ms; a waits-for edge runs from a waiting request
// to each other transaction holding an incompatible lock, or waiting for one
// ahead of it. If `waited` is non-null, Lock sets it to whether it blocked.
class LockManager {
 public:
  LockManager() = default;
  ~LockManager();

  auto Lock(Transaction* txn, const LockResource& resource, LockMode mode, SessionState* session,
            bool* waited = nullptr) -> bool;
  auto Unlock(Transaction* txn, const LockResource& resource) -> bool;
  void UnlockAll(Transaction* txn);
  void StartDeadlockDetection();
  void StopDeadlockDetection();
  auto GetLocks(txn_id_t id) const -> std::vector<std::pair<LockResource, LockMode>>;
  auto DebugString() const -> std::string;

 private:
  // Holders are identified by id: a granted request may outlive its
  // Transaction object if the caller never unlocks. The pointer is used only
  // while the request waits, when its transaction is necessarily alive.
  struct Request {
    txn_id_t id;
    Transaction* txn;
    LockMode mode;
    bool granted;
  };
  struct Queue {
    LockResource resource;
    std::list<Request> requests;
    std::condition_variable cv;
  };

  static auto KeyOf(const LockResource& resource) -> std::string;
  auto Grantable(const Queue& queue, std::list<Request>::const_iterator request) const -> bool;
  void DetectLoop();
  auto FindVictim() -> Transaction*;  // youngest transaction in some cycle, or null

  mutable std::mutex mu_;
  std::map<std::string, Queue> table_;
  std::map<txn_id_t, Queue*> waiting_;  // the queue each waiting transaction is in
  std::atomic<bool> detecting_{false};
  std::thread detector_;
};

}  // namespace ontodb
