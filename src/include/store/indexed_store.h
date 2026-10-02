#pragma once

#include "catalog/catalog.h"
#include "dictionary/dictionary.h"
#include "store/triple_store.h"

namespace ontodb {

// Triple store backed by B+ tree permutations. Insert and Delete update every
// index the catalog has a root for, or leave the store unchanged if the
// triple was already present or absent. Scan picks an index whose key order
// matches the bound prefix of the pattern. With only SPO, patterns that are
// not a prefix of SPO still return the right triples (a scan plus filter is
// allowed); they do not have to be fast. Iterators are IndexedTripleIterator.
class IndexedStore : public TripleStore {
 public:
  IndexedStore(Catalog* catalog, BufferPoolManager* bpm, Dictionary* dictionary);
  auto Insert(const Triple& triple) -> bool override;
  auto Delete(const Triple& triple) -> bool override;
  auto Scan(const TriplePattern& pattern) -> std::unique_ptr<TripleIterator> override;

 private:
  Catalog* catalog_;
  BufferPoolManager* bpm_;
  Dictionary* dictionary_;
};

}  // namespace ontodb
