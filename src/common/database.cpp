#include "common/database.h"

#include "binder/binder.h"
#include "common/exception.h"
#include "dictionary/mem_dictionary.h"
#include "parser/parser.h"
#include "store/mem_store.h"

namespace ontodb {

Database::Database()
    : dictionary_(std::make_unique<MemDictionary>()), store_(std::make_unique<MemStore>()) {}

auto Database::Load(const std::string& path) -> LoadStats {
  return RdfLoader().Load(path, *dictionary_, *store_);
}

auto Database::GetConfig() -> Config {
  std::lock_guard<std::mutex> guard(config_mu_);
  return config_;
}

void Database::UpdateConfig(const Config& config) {
  std::lock_guard<std::mutex> guard(config_mu_);
  config_ = config;
}

void Database::SetTracing(bool enabled) {
  trace_.SetEnabled(enabled);
}

auto Database::Execute(const std::string& sparql, SessionState* session) -> StatementResult {
  const AstQuery ast = Parser(sparql).Parse();
  const BoundQuery bound = Binder(*dictionary_).Bind(ast);
  const Config config = GetConfig();
  ExecutorContext ctx(store_.get(), dictionary_.get(), &config, nullptr, nullptr, nullptr, bpm_,
                      &stats_, &trace_, session);
  return ExecutionEngine(&ctx).Execute(bound);
}

auto Database::Explain(const std::string& sparql) -> std::string {
  const AstQuery ast = Parser(sparql).Parse();
  const BoundQuery bound = Binder(*dictionary_).Bind(ast);
  const Config config = GetConfig();
  ExecutorContext ctx(store_.get(), dictionary_.get(), &config, nullptr, nullptr, nullptr, bpm_,
                      &stats_, &trace_, nullptr);
  return ExecutionEngine(&ctx).Explain(bound);
}

auto Database::ExplainAnalyze(const std::string& sparql) -> std::string {
  const AstQuery ast = Parser(sparql).Parse();
  const BoundQuery bound = Binder(*dictionary_).Bind(ast);
  const Config config = GetConfig();
  ExecutorContext ctx(store_.get(), dictionary_.get(), &config, nullptr, nullptr, nullptr, bpm_,
                      &stats_, &trace_, nullptr);
  return ExecutionEngine(&ctx).ExplainAnalyze(bound);
}

auto Database::BufferPoolDebug() const -> std::string {
  throw NotImplementedException("PLAN 1.6: BufferPoolManager::DebugString");
}

auto Database::PageDebug(page_id_t) const -> std::string {
  throw NotImplementedException("PLAN 1.6: Shell::CmdPage");
}

auto Database::TreeDebug(const std::string&, bool) const -> std::string {
  throw NotImplementedException("PLAN 2.2: BPlusTree::ToString");
}

void Database::Begin() {
  throw NotImplementedException("PLAN 7.2: TransactionManager::Begin");
}

void Database::Commit() {
  throw NotImplementedException("PLAN 7.2: TransactionManager::Commit");
}

void Database::Abort() {
  throw NotImplementedException("PLAN 7.2: TransactionManager::Abort");
}

auto Database::TransactionsDebug() const -> std::string {
  throw NotImplementedException("PLAN 7.2: TransactionManager::DebugString");
}

auto Database::LocksDebug() const -> std::string {
  throw NotImplementedException("PLAN 7.3: LockManager::DebugString");
}

auto Database::LogDebug(size_t) const -> std::string {
  throw NotImplementedException("PLAN 8.2: LogManager::DebugTail");
}

void Database::Checkpoint() {
  throw NotImplementedException("PLAN 8.5: CheckpointManager::Checkpoint");
}

void Database::Crash() {
  throw NotImplementedException("PLAN 8.3: Database::Crash");
}

}  // namespace ontodb
