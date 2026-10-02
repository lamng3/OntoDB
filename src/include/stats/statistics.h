#pragma once

#include <cstdint>
#include <string_view>

#include "common/types.h"
#include "dictionary/dictionary.h"
#include "store/triple_store.h"

namespace ontodb {

// Counts gathered at load. These four are the minimum the estimator relies on.
// Add whatever else you need; do not remove these.
// TripleCount is the number of stored triples.
// DistinctCount("s" | "p" | "o") is the number of distinct terms in that
// position. PredicateCount is exact. PatternCount is the number of triples
// matching a pattern of constants (unbound positions are wildcards).
class Statistics {
 public:
  void Collect(const TripleStore& store, const Dictionary& dictionary);
  auto TripleCount() const -> uint64_t;
  auto DistinctCount(std::string_view position) const -> uint64_t;
  auto PredicateCount(term_id_t predicate) const -> uint64_t;
  auto PatternCount(const TriplePattern& pattern) const -> uint64_t;
};

}  // namespace ontodb
