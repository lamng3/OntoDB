#include "store/indexed_store.h"

#include <optional>
#include <stdexcept>

#include "common/exception.h"
#include "storage/page/b_plus_tree_header_page.h"
#include "store/indexed_triple_iterator.h"

namespace ontodb {

namespace {

constexpr std::array<const char*, 6> kAllNames = {"spo", "sop", "pso", "pos", "osp", "ops"};

// "pos" -> {1, 2, 0}: the key holds the predicate, then the object, then the subject.
auto OrderOf(std::string_view name) -> std::optional<std::array<int, 3>> {
  if (name.size() != 3) {
    return std::nullopt;
  }
  std::array<int, 3> order{};
  for (size_t i = 0; i < 3; i++) {
    const char c = name[i];
    order[i] = c == 's' ? 0 : c == 'p' ? 1 : c == 'o' ? 2 : -1;
    if (order[i] < 0) {
      return std::nullopt;
    }
  }
  if (order[0] == order[1] || order[1] == order[2] || order[0] == order[2]) {
    return std::nullopt;
  }
  return order;
}

auto Component(const Triple& t, int position) -> term_id_t {
  return position == 0 ? t.subject : position == 1 ? t.predicate : t.object;
}

auto Bound(const TriplePattern& p, int position) -> std::optional<term_id_t> {
  return position == 0 ? p.subject : position == 1 ? p.predicate : p.object;
}

}  // namespace

IndexedStore::IndexedStore(Catalog* catalog, BufferPoolManager* bpm, Dictionary* dictionary,
                           const std::vector<std::string>& permutations)
    : catalog_(catalog), bpm_(bpm), dictionary_(dictionary) {
  for (const char* name : kAllNames) {
    const page_id_t header = catalog_->GetIndexRoot(name);
    const auto order = OrderOf(name);
    if (header != INVALID_PAGE_ID && order.has_value()) {
      indexes_.push_back({name, *order, std::make_unique<BPlusTree>(name, header, bpm_)});
    }
  }
  if (!indexes_.empty()) {
    return;
  }
  if (permutations.empty()) {
    throw std::invalid_argument("an indexed store needs at least one permutation");
  }
  for (const auto& name : permutations) {
    const auto order = OrderOf(name);
    if (!order.has_value()) {
      throw std::invalid_argument("\"" + name + "\" is not a permutation of s, p, o");
    }
    page_id_t header = INVALID_PAGE_ID;
    {
      WritePageGuard guard = bpm_->NewPageGuarded(&header);
      BPlusTreeHeaderPage(guard.GetPage()).Init();
    }
    catalog_->SetIndexRoot(name, header);
    indexes_.push_back({name, *order, std::make_unique<BPlusTree>(name, header, bpm_)});
  }
}

// SPO, POS, and OSP make every bound/unbound pattern a prefix scan. The other
// three only add sort orders, which no operator uses; on LUBM(1) they cost 80%
// more space and saved nothing. See benchmarks/indexing/results.md.
auto IndexedStore::DefaultPermutations() -> std::vector<std::string> {
  return {"spo", "pos", "osp"};
}

auto IndexedStore::IndexNames() const -> std::vector<std::string> {
  std::vector<std::string> names;
  for (const auto& index : indexes_) {
    names.push_back(index.name);
  }
  return names;
}

auto IndexedStore::Tree(std::string_view name) -> BPlusTree* {
  for (auto& index : indexes_) {
    if (index.name == name) {
      return index.tree.get();
    }
  }
  return nullptr;
}

auto IndexedStore::KeyOf(const Index& index, const Triple& triple) const -> TripleKey {
  return TripleKey::Encode(Component(triple, index.order[0]), Component(triple, index.order[1]),
                           Component(triple, index.order[2]));
}

auto IndexedStore::Insert(const Triple& triple) -> bool {
  const Index& first = indexes_.front();
  if (first.tree->GetValue(KeyOf(first, triple))) {
    return false;
  }
  size_t done = 0;
  try {
    for (; done < indexes_.size(); done++) {
      indexes_[done].tree->Insert(KeyOf(indexes_[done], triple));
    }
  } catch (...) {
    while (done-- > 0) {  // put the indexes already written back
      indexes_[done].tree->Remove(KeyOf(indexes_[done], triple));
    }
    throw;
  }
  return true;
}

auto IndexedStore::Delete(const Triple& triple) -> bool {
  const Index& first = indexes_.front();
  if (!first.tree->GetValue(KeyOf(first, triple))) {
    return false;
  }
  size_t done = 0;
  try {
    for (; done < indexes_.size(); done++) {
      indexes_[done].tree->Remove(KeyOf(indexes_[done], triple));
    }
  } catch (...) {
    while (done-- > 0) {
      indexes_[done].tree->Insert(KeyOf(indexes_[done], triple));
    }
    throw;
  }
  return true;
}

auto IndexedStore::Scan(const TriplePattern& pattern) -> std::unique_ptr<TripleIterator> {
  const Index* best = nullptr;
  int best_prefix = -1;
  for (const auto& index : indexes_) {
    int prefix = 0;
    while (prefix < 3 && Bound(pattern, index.order[prefix]).has_value()) {
      prefix++;
    }
    if (prefix > best_prefix) {
      best = &index;
      best_prefix = prefix;
    }
  }
  if (best == nullptr) {
    throw std::logic_error("an indexed store has no index");
  }
  std::array<term_id_t, 3> start{0, 0, 0};
  for (int i = 0; i < best_prefix; i++) {
    start[i] = Bound(pattern, best->order[i]).value_or(0);  // bound: it is in the prefix
  }
  return std::make_unique<IndexedTripleIterator>(best->tree.get(),
                                                 TripleKey::Encode(start[0], start[1], start[2]),
                                                 best_prefix, best->order, pattern);
}

IndexedTripleIterator::IndexedTripleIterator(BPlusTree* tree, const TripleKey& start,
                                             int prefix_len, std::array<int, 3> order,
                                             TriplePattern filter)
    : tree_(tree), start_(start), prefix_len_(prefix_len), order_(order), filter_(filter) {}

void IndexedTripleIterator::Init() {
  cursor_ = IndexIterator{};  // drop the old leaf before pinning the first one again
  cursor_ = tree_->Begin(start_, prefix_len_);
  started_ = true;
}

auto IndexedTripleIterator::Next(Triple* out) -> bool {
  if (!started_) {
    Init();
  }
  while (!cursor_.IsEnd()) {
    std::array<term_id_t, 3> key{};
    (*cursor_).Decode(&key[0], &key[1], &key[2]);
    ++cursor_;
    examined_++;
    Triple triple;
    for (int i = 0; i < 3; i++) {
      (order_[i] == 0   ? triple.subject
       : order_[i] == 1 ? triple.predicate
                        : triple.object) = key[i];
    }
    if (Matches(triple, filter_)) {
      *out = triple;
      return true;
    }
  }
  return false;
}

}  // namespace ontodb
