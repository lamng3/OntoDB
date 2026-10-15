#include "common/database.h"

#include <cstdlib>
#include <iomanip>
#include <sstream>

#include "binder/binder.h"
#include "buffer/buffer_pool_manager.h"
#include "common/exception.h"
#include "dictionary/mem_dictionary.h"
#include "parser/parser.h"
#include "store/mem_store.h"

namespace ontodb {

Database::Database()
    : dictionary_(std::make_unique<MemDictionary>()), store_(std::make_unique<MemStore>()) {}

Database::~Database() = default;

auto Database::Open(const std::filesystem::path& dir, RecoveryMode mode, OpenReport* report)
    -> std::unique_ptr<Database> {
  auto db = std::make_unique<Database>();
  db->durability_ =
      std::make_unique<Durability>(dir, db->dictionary_.get(), db->store_.get(), &db->stats_);
  const OpenReport opened = db->durability_->Open(mode);
  if (report != nullptr) {
    *report = opened;
  }
  return db;
}

auto Database::Load(const std::string& path) -> LoadStats {
  if (!durability_) {
    return RdfLoader().Load(path, *dictionary_, *store_);
  }
  LoadStats stats;
  durability_->Run(
      [&](TripleStore& store) { stats = RdfLoader().Load(path, *dictionary_, store); });
  return stats;
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
  const bool is_update =
      bound.kind == BoundQuery::Kind::kInsert || bound.kind == BoundQuery::Kind::kDelete;
  if (!durability_ || !is_update) {
    ExecutorContext ctx(store_.get(), dictionary_.get(), &config, nullptr, nullptr, nullptr, bpm_,
                        &stats_, &trace_, session);
    return ExecutionEngine(&ctx).Execute(bound);
  }
  StatementResult result;
  durability_->Run([&](TripleStore& store) {
    ExecutorContext ctx(&store, dictionary_.get(), &config, nullptr, nullptr, nullptr, bpm_,
                        &stats_, &trace_, session);
    result = ExecutionEngine(&ctx).Execute(bound);
  });
  return result;
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

namespace {

constexpr char kNoPool[] =
    "no buffer pool: the memory backend keeps triples in memory; the indexed backend (PLAN 3.4) "
    "has one";

}  // namespace

auto Database::BufferPoolDebug() const -> std::string {
  if (bpm_ == nullptr) {
    return kNoPool;
  }
  return bpm_->DebugString();
}

// The pageLSN, then a hex dump of the first 64 payload bytes. Reading the page
// pins it for the length of this call only.
auto Database::PageDebug(page_id_t page_id) const -> std::string {
  if (bpm_ == nullptr) {
    return kNoPool;
  }
  ReadPageGuard guard = bpm_->FetchPageRead(page_id);
  const char* data = guard.GetData();
  lsn_t lsn = 0;
  for (size_t i = 0; i < kPageLsnSize; i++) {
    lsn |= static_cast<lsn_t>(static_cast<unsigned char>(data[kPageLsnOffset + i])) << (8 * i);
  }
  std::ostringstream out;
  out << "page " << page_id << ": pageLSN " << lsn << "\n";
  constexpr size_t kDumpBytes = 64;
  constexpr size_t kRow = 16;
  for (size_t row = 0; row < kDumpBytes; row += kRow) {
    out << "  +" << std::setw(4) << std::setfill('0') << (kPagePayloadOffset + row) << ' ';
    for (size_t i = 0; i < kRow; i++) {
      out << ' ' << std::hex << std::setw(2) << std::setfill('0')
          << static_cast<int>(static_cast<unsigned char>(data[kPagePayloadOffset + row + i]))
          << std::dec;
    }
    out << '\n';
  }
  return out.str();
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

auto Database::LogDebug(size_t n) const -> std::string {
  if (durability_) {
    return durability_->LogTail(n);
  }
  throw NotImplementedException("PLAN 8.2: LogManager::DebugTail");
}

auto Database::Checkpoint() -> std::string {
  if (durability_) {
    return durability_->Checkpoint();
  }
  throw NotImplementedException("PLAN 8.5: CheckpointManager::Checkpoint");
}

auto Database::Verify() -> std::string {
  if (durability_) {
    return durability_->Verify();
  }
  return "error: nothing on disk to verify; start the shell with a directory (ontodb <dir>)";
}

void Database::Crash() {
  if (durability_) {
    // Exit without destructors, so nothing buffered reaches the disk.
    std::_Exit(0);
  }
  throw NotImplementedException("PLAN 8.3: Database::Crash");
}

}  // namespace ontodb
