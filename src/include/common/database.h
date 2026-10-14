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
#include "store/mem_store.h"

namespace ontodb {

class BufferPoolManager;

// In-memory database used by the shell until the indexed backend exists.
// `Database()` keeps nothing on disk. `Database::Open(dir)` makes it durable
// (PLAN 9): every update statement and every \load is a logged transaction,
// and reopening the directory restores the committed state.
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

  auto GetDictionary() -> Dictionary* { return dictionary_.get(); }
  auto GetStore() -> TripleStore* { return store_.get(); }
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
  std::unique_ptr<MemDictionary> dictionary_;
  std::unique_ptr<MemStore> store_;
  IoStats stats_;
  SwitchableTraceSink trace_;
  Config config_;
  std::mutex config_mu_;
  BufferPoolManager* bpm_{nullptr};
  std::unique_ptr<Durability> durability_;
};

}  // namespace ontodb
