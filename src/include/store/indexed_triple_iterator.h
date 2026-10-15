#pragma once

#include <array>

#include "storage/index/b_plus_tree.h"
#include "storage/index/index_iterator.h"
#include "store/triple_iterator.h"

namespace ontodb {

class IndexedStore;

// TripleIterator over one index range. It owns an IndexIterator, so it holds
// one leaf guard, and its destructor releases that guard.
//
// `order[i]` is the triple position (0 subject, 1 predicate, 2 object) stored
// in key component i of this index. The range is the `prefix_len`-prefix of
// `start`; bound positions of `filter` outside that prefix are checked on each
// triple, which is how a pattern that is not a prefix of the index is served.
// Init (re)opens the range; Next calls Init if it has not run.
class IndexedTripleIterator : public TripleIterator {
 public:
  IndexedTripleIterator(BPlusTree* tree, const TripleKey& start, int prefix_len,
                        std::array<int, 3> order, TriplePattern filter);
  void Init() override;
  auto Next(Triple* out) -> bool override;
  // Keys read from the index since construction, matched or filtered out.
  // Benchmarks use it to see how much of a scan was wasted on the filter.
  auto KeysExamined() const -> uint64_t { return examined_; }

 private:
  BPlusTree* tree_;
  TripleKey start_;
  int prefix_len_{0};
  std::array<int, 3> order_;
  TriplePattern filter_;
  IndexIterator cursor_;
  bool started_{false};
  uint64_t examined_{0};
};

}  // namespace ontodb
