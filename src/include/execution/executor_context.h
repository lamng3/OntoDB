#pragma once

#include <chrono>
#include <optional>

#include "common/exception.h"
#include "common/io_stats.h"
#include "common/session_state.h"
#include "common/trace.h"
#include "common/types.h"

namespace ontodb {

class TripleStore;
class Dictionary;
class Transaction;
class TransactionManager;
class LockManager;
class BufferPoolManager;
class TypeEntailment;

// Everything an executor is allowed to touch. Transaction, lock, and buffer
// pointers stay null on the memory backend.
class ExecutorContext {
 public:
  ExecutorContext(TripleStore* store, Dictionary* dictionary, const Config* config,
                  Transaction* txn, TransactionManager* txn_mgr, LockManager* lock_mgr,
                  BufferPoolManager* bpm, IoStats* stats, TraceSink* trace, SessionState* session)
      : store_(store)
      , dictionary_(dictionary)
      , config_(config)
      , txn_(txn)
      , txn_mgr_(txn_mgr)
      , lock_mgr_(lock_mgr)
      , bpm_(bpm)
      , stats_(stats)
      , trace_(trace)
      , session_(session) {}

  auto GetStore() const -> TripleStore* { return store_; }
  auto GetDictionary() const -> Dictionary* { return dictionary_; }
  auto GetConfig() const -> const Config* { return config_; }
  auto GetTransaction() const -> Transaction* { return txn_; }
  auto GetTransactionManager() const -> TransactionManager* { return txn_mgr_; }
  auto GetLockManager() const -> LockManager* { return lock_mgr_; }
  auto GetBufferPool() const -> BufferPoolManager* { return bpm_; }
  auto GetStats() const -> IoStats* { return stats_; }
  auto GetTrace() const -> TraceSink* { return trace_; }
  auto GetSession() const -> SessionState* { return session_; }
  // Set when \set entail is on and the entailment index is built; null otherwise.
  void SetEntailment(TypeEntailment* entailment) { entailment_ = entailment; }
  auto GetEntailment() const -> TypeEntailment* { return entailment_; }

  // An optional wall-clock deadline. Scans call CheckDeadline as they work
  // and on every re-open, so a plan of any shape stops soon after it passes.
  void SetDeadline(std::chrono::steady_clock::time_point deadline) { deadline_ = deadline; }
  void CheckDeadline() const {
    if (deadline_.has_value() && std::chrono::steady_clock::now() > *deadline_) {
      throw QueryTimeoutException("query timed out");
    }
  }

 private:
  TripleStore* store_;
  Dictionary* dictionary_;
  const Config* config_;
  Transaction* txn_;
  TransactionManager* txn_mgr_;
  LockManager* lock_mgr_;
  BufferPoolManager* bpm_;
  IoStats* stats_;
  TraceSink* trace_;
  SessionState* session_;
  TypeEntailment* entailment_{nullptr};
  std::optional<std::chrono::steady_clock::time_point> deadline_;
};

}  // namespace ontodb
