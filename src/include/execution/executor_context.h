#pragma once

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
};

}  // namespace ontodb
