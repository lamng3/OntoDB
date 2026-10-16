#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

#include "common/io_stats.h"
#include "common/session_state.h"
#include "common/trace.h"
#include "common/types.h"
#include "dictionary/mem_dictionary.h"
#include "execution/execution_engine.h"
#include "loader/rdf_loader.h"
#include "recovery/durability.h"
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
// Statements on the indexed backend run one at a time: the tree is safe for
// concurrent use (PLAN 7.1), but a scan holds a leaf latch between rows, and
// a transaction waiting for a lock while holding a latch could deadlock with
// a writer in a way the lock manager cannot see.
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

  std::unique_ptr<MemDictionary> dictionary_;
  std::unique_ptr<MemStore> store_;
  IoStats stats_;
  SwitchableTraceSink trace_;
  Config config_;
  std::mutex config_mu_;
  BufferPoolManager* bpm_{nullptr};
  std::unique_ptr<Durability> durability_;
  std::unique_ptr<IndexedBackend> indexed_;
  std::mutex indexed_mu_;  // one statement at a time on the indexed backend
  std::mutex statistics_mu_;
  std::shared_ptr<const Statistics> statistics_;
};

}  // namespace ontodb
