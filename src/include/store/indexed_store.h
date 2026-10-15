#pragma once

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "catalog/catalog.h"
#include "dictionary/dictionary.h"
#include "storage/index/b_plus_tree.h"
#include "store/triple_store.h"

namespace ontodb {

// Triple store backed by B+ tree permutations. Insert and Delete update every
// index the catalog has a root for, or leave the store unchanged if the
// triple was already present or absent. Scan picks an index whose key order
// matches the bound prefix of the pattern. With only SPO, patterns that are
// not a prefix of SPO still return the right triples (a scan plus filter is
// allowed); they do not have to be fast. Iterators are IndexedTripleIterator.
//
// A permutation is named by its key order: "spo", "sop", "pso", "pos", "osp",
// "ops". On a catalog with none of them, the store creates `permutations`
// (DefaultPermutations() unless given) and records each tree's header page in
// the catalog; on a catalog that has some, it uses exactly those. Scan uses
// the index with the longest bound prefix, the first listed on a tie. If an
// index throws part way through Insert or Delete, the indexes already changed
// are put back before the exception propagates. Single-threaded until latch
// crabbing (PLAN 7.1).
class IndexedStore : public TripleStore {
 public:
  IndexedStore(Catalog* catalog, BufferPoolManager* bpm, Dictionary* dictionary,
               const std::vector<std::string>& permutations = DefaultPermutations());
  auto Insert(const Triple& triple) -> bool override;
  auto Delete(const Triple& triple) -> bool override;
  auto Scan(const TriplePattern& pattern) -> std::unique_ptr<TripleIterator> override;

  static auto DefaultPermutations() -> std::vector<std::string>;
  auto IndexNames() const -> std::vector<std::string>;
  auto Tree(std::string_view name) -> BPlusTree*;  // nullptr when absent

 private:
  struct Index {
    std::string name;
    std::array<int, 3> order;  // triple position held by each key component
    std::unique_ptr<BPlusTree> tree;
  };

  auto KeyOf(const Index& index, const Triple& triple) const -> TripleKey;

  Catalog* catalog_;
  BufferPoolManager* bpm_;
  Dictionary* dictionary_;
  std::vector<Index> indexes_;
};

}  // namespace ontodb
