#include "common/database.h"

#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <sstream>

#include "binder/binder.h"
#include "buffer/buffer_pool_manager.h"
#include "catalog/catalog.h"
#include "common/exception.h"
#include "dictionary/disk_dictionary.h"
#include "dictionary/mem_dictionary.h"
#include "parser/parser.h"
#include "store/indexed_store.h"
#include "store/mem_store.h"

namespace ontodb {

// The indexed backend's parts, in construction order. The file is deleted
// after everything that uses it is gone (members are destroyed in reverse).
struct Database::IndexedBackend {
  struct TempFile {
    std::filesystem::path path;
    ~TempFile() {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  };

  IndexedBackend(std::filesystem::path file, const Config& config, IoStats* stats, TraceSink* trace)
      : temp{std::move(file)}
      , disk(temp.path.string(), stats, trace)
      , pool(config.pool_size, &disk, config.lru_k, nullptr, stats, trace)
      , catalog(&pool)
      , dictionary(&pool, &catalog)
      , store(&catalog, &pool, &dictionary) {}

  TempFile temp;
  DiskManager disk;
  BufferPoolManager pool;
  Catalog catalog;
  DiskDictionary dictionary;
  IndexedStore store;
};

namespace {

auto TempDatabasePath() -> std::filesystem::path {
  static std::atomic<int> counter{0};
  return std::filesystem::temp_directory_path() /
         ("ontodb-" + std::to_string(::getpid()) + "-" + std::to_string(counter++) + ".db");
}

// Copies every triple by spelling, so the target dictionary assigns its own ids.
void CopyTriples(Dictionary& from_terms, TripleStore& from, Dictionary& to_terms, TripleStore& to) {
  auto scan = from.Scan(TriplePattern{});
  scan->Init();
  Triple triple;
  while (scan->Next(&triple)) {
    auto id = [&](term_id_t old) { return to_terms.Insert(*from_terms.Lookup(old)); };
    to.Insert(Triple{id(triple.subject), id(triple.predicate), id(triple.object)});
  }
}

}  // namespace

Database::Database()
    : dictionary_(std::make_unique<MemDictionary>()), store_(std::make_unique<MemStore>()) {}

Database::~Database() = default;

auto Database::CurrentStatistics() -> std::shared_ptr<const Statistics> {
  std::lock_guard<std::mutex> guard(statistics_mu_);
  if (statistics_ == nullptr) {
    auto fresh = std::make_shared<Statistics>();
    fresh->Collect(*GetStore(), *GetDictionary());
    statistics_ = std::move(fresh);
  }
  return statistics_;
}

void Database::InvalidateStatistics() {
  std::lock_guard<std::mutex> guard(statistics_mu_);
  statistics_.reset();
}

auto Database::GetDictionary() -> Dictionary* {
  return indexed_ ? static_cast<Dictionary*>(&indexed_->dictionary) : dictionary_.get();
}

auto Database::GetStore() -> TripleStore* {
  return indexed_ ? static_cast<TripleStore*>(&indexed_->store) : store_.get();
}

void Database::SetBackend(BackendKind kind) {
  std::lock_guard<std::mutex> guard(indexed_mu_);
  if (kind == BackendKind::kIndexed && !indexed_) {
    if (durability_) {
      throw std::runtime_error(
          "a persistent database stays on the memory backend until page-level recovery (PLAN 8)");
    }
    auto backend =
        std::make_unique<IndexedBackend>(TempDatabasePath(), GetConfig(), &stats_, &trace_);
    CopyTriples(*dictionary_, *store_, backend->dictionary, backend->store);
    indexed_ = std::move(backend);
    bpm_ = &indexed_->pool;
    dictionary_ = std::make_unique<MemDictionary>();
    store_ = std::make_unique<MemStore>();
  } else if (kind == BackendKind::kMem && indexed_) {
    auto terms = std::make_unique<MemDictionary>();
    auto triples = std::make_unique<MemStore>();
    CopyTriples(indexed_->dictionary, indexed_->store, *terms, *triples);
    dictionary_ = std::move(terms);
    store_ = std::move(triples);
    bpm_ = nullptr;
    indexed_.reset();
  }
  Config config = GetConfig();
  config.backend = kind;
  UpdateConfig(config);
  InvalidateStatistics();
}

auto Database::Open(const std::filesystem::path& dir, RecoveryMode mode,
                    OpenReport* report) -> std::unique_ptr<Database> {
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
    std::unique_lock<std::mutex> one_at_a_time(indexed_mu_, std::defer_lock);
    if (indexed_) {
      one_at_a_time.lock();
    }
    const LoadStats stats = RdfLoader().Load(path, *GetDictionary(), *GetStore());
    InvalidateStatistics();
    return stats;
  }
  LoadStats stats;
  durability_->Run(
      [&](TripleStore& store) { stats = RdfLoader().Load(path, *dictionary_, store); });
  InvalidateStatistics();
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
  std::unique_lock<std::mutex> one_at_a_time(indexed_mu_, std::defer_lock);
  if (indexed_) {
    one_at_a_time.lock();
  }
  const AstQuery ast = Parser(sparql).Parse();
  const BoundQuery bound = Binder(*GetDictionary()).Bind(ast);
  const Config config = GetConfig();
  const bool is_update =
      bound.kind == BoundQuery::Kind::kInsert || bound.kind == BoundQuery::Kind::kDelete;
  if (!durability_ || !is_update) {
    ExecutorContext ctx(GetStore(), GetDictionary(), &config, nullptr, nullptr, nullptr, bpm_,
                        &stats_, &trace_, session);
    const auto statistics = is_update ? nullptr : CurrentStatistics();
    return ExecutionEngine(&ctx, statistics.get()).Execute(bound);
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
  std::unique_lock<std::mutex> one_at_a_time(indexed_mu_, std::defer_lock);
  if (indexed_) {
    one_at_a_time.lock();
  }
  const AstQuery ast = Parser(sparql).Parse();
  const BoundQuery bound = Binder(*GetDictionary()).Bind(ast);
  const Config config = GetConfig();
  ExecutorContext ctx(GetStore(), GetDictionary(), &config, nullptr, nullptr, nullptr, bpm_,
                      &stats_, &trace_, nullptr);
  const auto statistics = CurrentStatistics();
  return ExecutionEngine(&ctx, statistics.get()).Explain(bound);
}

auto Database::ExplainAnalyze(const std::string& sparql) -> std::string {
  std::unique_lock<std::mutex> one_at_a_time(indexed_mu_, std::defer_lock);
  if (indexed_) {
    one_at_a_time.lock();
  }
  const AstQuery ast = Parser(sparql).Parse();
  const BoundQuery bound = Binder(*GetDictionary()).Bind(ast);
  const Config config = GetConfig();
  ExecutorContext ctx(GetStore(), GetDictionary(), &config, nullptr, nullptr, nullptr, bpm_,
                      &stats_, &trace_, nullptr);
  const auto statistics = CurrentStatistics();
  return ExecutionEngine(&ctx, statistics.get()).ExplainAnalyze(bound);
}

namespace {

constexpr char kNoPool[] =
    "no buffer pool: the memory backend keeps triples in memory; \\set backend indexed builds "
    "one";

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

auto Database::TreeDebug(const std::string& index, bool dot) const -> std::string {
  if (!indexed_) {
    return "no index: the memory backend keeps triples in memory; \\set backend indexed builds "
           "the B+ trees";
  }
  BPlusTree* tree = indexed_->store.Tree(index);
  if (tree == nullptr) {
    std::string names;
    for (const auto& name : indexed_->store.IndexNames()) {
      names += (names.empty() ? "" : ", ") + name;
    }
    return "no index named " + index + "; this store has " + names;
  }
  return dot ? tree->ToDot() : tree->ToString();
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
