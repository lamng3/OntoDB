#pragma once

#include "storage/index/index_iterator.h"
#include "store/triple_iterator.h"

namespace ontodb {

class IndexedStore;

// TripleIterator over one index range. It owns an IndexIterator, so it holds
// one leaf guard, and its destructor releases that guard.
class IndexedTripleIterator : public TripleIterator {
 public:
  IndexedTripleIterator(IndexIterator cursor, IndexIterator end, int prefix_len);
  void Init() override;
  auto Next(Triple* out) -> bool override;

 private:
  IndexIterator cursor_;
  IndexIterator end_;
  int prefix_len_{0};
  bool started_{false};
};

}  // namespace ontodb
