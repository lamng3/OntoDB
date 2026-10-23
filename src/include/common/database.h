#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "common/io_stats.h"
#include "common/session_state.h"
#include "common/trace.h"
#include "common/types.h"
#include "concurrency/lock_manager.h"
#include "concurrency/transaction_manager.h"
#include "dictionary/mem_dictionary.h"
#include "execution/execution_engine.h"
#include "loader/rdf_loader.h"
#include "recovery/durability.h"
#include "semantic/semantic_engine.h"
#include "semantic/type_entailment_index.h"
#include "stats/statistics.h"
#include "store/mem_store.h"

namespace ontodb {

class BufferPoolManager;

// The database the shell talks to. `Database()` keeps nothing on disk.
// `Database::Open(dir)` makes it durable (PLAN 9): every update statement and
// every \load is a logged transaction, and reopening the directory restores
// the committed state.
//
// Two backends hold the triples. The memory backend (the default) is a
// MemDictionary and a MemStore. SetBackend(kIndexed) builds the indexed
// backend in a temporary database file: a buffer pool of the configured size
// and k, the catalog, a DiskDictionary, and an IndexedStore. It moves every
// triple across by spelling, so ids are those of the new dictionary.
// SetBackend(kMem) moves them back and deletes the file. A persistent
// database stays on the memory backend until page-level recovery (PLAN 8).
// Concurrency (PLAN 7): each session thread has at most one explicit
// transaction (\begin ... \commit or \abort); a statement outside one runs in
// its own implicit transaction. Statements lock triples through the lock
// manager on both backends (strict two-phase locking, deadlock detection);
// the isolation levels are described in execution/executors. \load takes an
// exclusive lock on the whole database instead of one lock per triple. A
// persistent database does not accept \begin yet: its log commits each
// statement on its own. Switching backends waits for running statements and
// is refused while a transaction is open.
class Database {
 public:
  Database();
  ~Database();

  static auto Open(const std::filesystem::path& dir, RecoveryMode mode = RecoveryMode::kPointInTime,
                   OpenReport* report = nullptr) -> std::unique_ptr<Database>;
  auto IsDurable() const -> bool { return durability_ != nullptr; }

  auto Load(const std::string& path) -> LoadStats;
  auto Execute(const std::string& sparql, SessionState* session) -> StatementResult;
  auto Explain(const std::string& sparql) -> std::string;
  auto ExplainAnalyze(const std::string& sparql) -> std::string;

  auto GetDictionary() -> Dictionary*;
  auto GetStore() -> TripleStore*;
  void SetBackend(BackendKind kind);
  auto GetStats() -> IoStats& { return stats_; }
  auto GetTrace() -> TraceSink* { return &trace_; }
  auto GetBufferPool() -> BufferPoolManager* { return bpm_; }
  // The semantic search engine (PLAN 14), made on first use. Its labels and
  // vectors name terms by the dictionary's ids, so a backend switch drops it.
  auto Semantic() -> SemanticEngine&;

  auto GetConfig() -> Config;
  void UpdateConfig(const Config& config);
  void SetTracing(bool enabled);

  auto BufferPoolDebug() const -> std::string;
  auto PageDebug(page_id_t page_id) const -> std::string;
  auto TreeDebug(const std::string& index, bool dot) const -> std::string;
  void Begin();
  void Commit();
  void Abort();
  auto TransactionsDebug() const -> std::string;
  auto LocksDebug() const -> std::string;
  auto LogDebug(size_t n) const -> std::string;
  auto Checkpoint() -> std::string;
  auto Verify() -> std::string;
  void Crash();

 private:
  struct IndexedBackend;

  // The statistics the optimizer plans with: collected on first use after a
  // load or a backend switch, and not refreshed by updates (like a table not
  // re-ANALYZEd). Sessions share one immutable snapshot.
  auto CurrentStatistics() -> std::shared_ptr<const Statistics>;
  void InvalidateStatistics();
  auto CurrentTransaction() -> Transaction*;  // this thread's explicit transaction, or null

  std::unique_ptr<MemDictionary> dictionary_;
  std::unique_ptr<MemStore> store_;
  IoStats stats_;
  SwitchableTraceSink trace_;
  Config config_;
  std::mutex config_mu_;
  BufferPoolManager* bpm_{nullptr};
  std::unique_ptr<Durability> durability_;
  std::unique_ptr<IndexedBackend> indexed_;
  std::unique_ptr<SemanticEngine> semantic_;
  // The RDFS entailment of rdf:type patterns (\set entail on): a snapshot of the hierarchy and the
  // instances, rebuilt when data_version_ has moved. Built on first use.
  auto EntailmentFor(const Config& config) -> TypeEntailment*;
  std::mutex entailment_mu_;
  std::unique_ptr<TypeEntailmentIndex> entailment_;
  std::atomic<uint64_t> data_version_{0};
  std::shared_mutex backend_mu_;  // statements share it; a backend switch takes it alone
  LockManager locks_;
  std::unique_ptr<TransactionManager> txns_;  // over the active store
  std::mutex sessions_mu_;
  std::unordered_map<std::thread::id, Transaction*> current_;  // explicit transaction per thread
  std::mutex statistics_mu_;
  std::shared_ptr<const Statistics> statistics_;
};

}  // namespace ontodb
