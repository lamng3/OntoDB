#include "semantic/type_entailment_index.h"

#include <atomic>
#include <filesystem>
#include <unistd.h>

#include "semantic/type_closure.h"
#include "storage/disk/disk_manager.h"

namespace ontodb {

namespace {

constexpr char kType[] = "<http://www.w3.org/1999/02/22-rdf-syntax-ns#type>";
constexpr char kSubClassOf[] = "<http://www.w3.org/2000/01/rdf-schema#subClassOf>";
constexpr size_t kPoolFrames = 4096;
constexpr size_t kPoolShards = 16;

}  // namespace

struct TypeEntailmentIndex::Parts {
  std::filesystem::path file;
  std::unique_ptr<DiskManager> disk;
  std::unique_ptr<BufferPoolManager> pool;
  std::unique_ptr<ClassHierarchy> hierarchy;
  std::unique_ptr<LabelIndex> instances;
  std::vector<size_t> count_through;  // count_through[l]: type triples whose class label is at most l

  Parts() {
    static std::atomic<int> counter{0};
    file = std::filesystem::temp_directory_path() /
           ("ontodb-entail-" + std::to_string(::getpid()) + "-" + std::to_string(counter++) + ".db");
    disk = std::make_unique<DiskManager>(file.string());
    pool = std::make_unique<BufferPoolManager>(kPoolFrames, disk.get(), 2, nullptr, nullptr, nullptr, kPoolShards);
  }

  ~Parts() {
    instances.reset();
    pool.reset();
    disk.reset();
    std::error_code ignored;
    std::filesystem::remove(file, ignored);
  }
};

TypeEntailmentIndex::TypeEntailmentIndex() = default;
TypeEntailmentIndex::~TypeEntailmentIndex() = default;

void TypeEntailmentIndex::Refresh(TripleStore* store, Dictionary* dictionary, uint64_t version) {
  {
    std::shared_lock<std::shared_mutex> read(mu_);
    if (built_ && store_ == store && version_ == version) {
      return;
    }
  }
  std::unique_lock<std::shared_mutex> write(mu_);
  if (built_ && store_ == store && version_ == version) {
    return;
  }
  auto parts = std::make_unique<Parts>();
  const auto type = dictionary->Lookup(kType);
  const auto sub = dictionary->Lookup(kSubClassOf);
  type_ = type.value_or(INVALID_TERM_ID);
  std::vector<std::pair<term_id_t, term_id_t>> edges;
  std::vector<term_id_t> typed;
  if (sub.has_value()) {
    edges = CollectSubClassEdges(store, *sub);
  }
  if (type.has_value()) {
    typed = CollectTypeClasses(store, *type);
  }
  parts->hierarchy = std::make_unique<ClassHierarchy>(ClassHierarchy::Build(edges, ClassHierarchy::TreeParent::kDeepest, typed));
  parts->instances = std::make_unique<LabelIndex>(parts->pool.get());
  std::vector<std::pair<uint32_t, term_id_t>> pairs;
  if (type.has_value()) {
    pairs = TypeLabelPairs(*parts->hierarchy, store, *type);
  }
  parts->count_through.assign(parts->hierarchy->MaxLabel() + 1, 0);
  for (const auto& [label, instance] : pairs) {
    (void)instance;
    parts->count_through[label]++;
  }
  for (size_t l = 1; l < parts->count_through.size(); l++) {
    parts->count_through[l] += parts->count_through[l - 1];
  }
  parts->instances->BulkLoad(std::move(pairs));
  parts->pool->FlushAllPages();
  parts_ = std::move(parts);
  store_ = store;
  version_ = version;
  built_ = true;
}

auto TypeEntailmentIndex::IndexBytes() const -> uint64_t {
  std::shared_lock<std::shared_mutex> read(mu_);
  if (!built_) {
    return 0;
  }
  std::error_code ignored;
  return static_cast<uint64_t>(std::filesystem::file_size(parts_->file, ignored));
}

auto TypeEntailmentIndex::RdfType() const -> term_id_t {
  std::shared_lock<std::shared_mutex> read(mu_);
  return built_ ? type_ : INVALID_TERM_ID;
}

auto TypeEntailmentIndex::Covers(term_id_t cls) const -> bool {
  std::shared_lock<std::shared_mutex> read(mu_);
  return built_ && parts_->hierarchy->Contains(cls);
}

auto TypeEntailmentIndex::EstimateInstances(term_id_t cls) const -> double {
  std::shared_lock<std::shared_mutex> read(mu_);
  if (!built_ || !parts_->hierarchy->Contains(cls)) {
    return 0;
  }
  size_t total = 0;
  for (const Interval& interval : parts_->hierarchy->Intervals(cls)) {
    total += parts_->count_through[interval.hi] - parts_->count_through[interval.lo - 1];
  }
  return static_cast<double>(total);
}

auto TypeEntailmentIndex::Instances(term_id_t cls) const -> std::vector<term_id_t> {
  std::shared_lock<std::shared_mutex> read(mu_);
  if (!built_ || !parts_->hierarchy->Contains(cls)) {
    return {};
  }
  return InstancesByIntervals(*parts_->hierarchy, parts_->instances.get(), cls);
}

auto TypeEntailmentIndex::IsInstance(term_id_t entity, term_id_t cls) const -> bool {
  std::shared_lock<std::shared_mutex> read(mu_);
  if (!built_ || !parts_->hierarchy->Contains(cls)) {
    return false;
  }
  auto scan = store_->Scan(TriplePattern{entity, type_, std::nullopt});
  scan->Init();
  Triple t;
  while (scan->Next(&t)) {
    if (t.object == cls || (parts_->hierarchy->Contains(t.object) && parts_->hierarchy->IsDescendant(t.object, cls))) {
      return true;
    }
  }
  return false;
}

}  // namespace ontodb
