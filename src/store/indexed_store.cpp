#include "store/indexed_store.h"

#include "common/exception.h"
#include "store/indexed_triple_iterator.h"

namespace ontodb {

IndexedStore::IndexedStore(Catalog* catalog, BufferPoolManager* bpm, Dictionary* dictionary)
    : catalog_(catalog), bpm_(bpm), dictionary_(dictionary) {}

auto IndexedStore::Insert(const Triple&) -> bool {
  throw NotImplementedException("PLAN 3.4: IndexedStore::Insert");
}
auto IndexedStore::Delete(const Triple&) -> bool {
  throw NotImplementedException("PLAN 3.4: IndexedStore::Delete");
}
auto IndexedStore::Scan(const TriplePattern&) -> std::unique_ptr<TripleIterator> {
  throw NotImplementedException("PLAN 3.4: IndexedStore::Scan");
}

IndexedTripleIterator::IndexedTripleIterator(IndexIterator cursor, IndexIterator end,
                                             int prefix_len)
    : cursor_(std::move(cursor)), end_(std::move(end)), prefix_len_(prefix_len) {}

void IndexedTripleIterator::Init() {
  throw NotImplementedException("PLAN 3.4: IndexedTripleIterator::Init");
}
auto IndexedTripleIterator::Next(Triple*) -> bool {
  throw NotImplementedException("PLAN 3.4: IndexedTripleIterator::Next");
}

}  // namespace ontodb
