#pragma once

#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

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
//
// Collect scans the store once and keeps exact counts for every single
// position, every pair of positions, and the set of whole triples, so
// PatternCount is exact for every combination of bound positions. The space
// is about seven hash entries per triple. Counts are a snapshot: the store may change after
// Collect, and the counts do not follow it.
class Statistics {
 public:
  void Collect(TripleStore& store, const Dictionary& dictionary);
  auto TripleCount() const -> uint64_t;
  auto DistinctCount(std::string_view position) const -> uint64_t;
  auto PredicateCount(term_id_t predicate) const -> uint64_t;
  auto PatternCount(const TriplePattern& pattern) const -> uint64_t;

 private:
  struct TripleHash {
    auto operator()(const Triple& t) const noexcept -> size_t;
  };
  struct PairHash {
    auto operator()(const std::pair<term_id_t, term_id_t>& pair) const noexcept -> size_t;
  };
  using Counts = std::unordered_map<term_id_t, uint64_t>;
  using PairCounts = std::unordered_map<std::pair<term_id_t, term_id_t>, uint64_t, PairHash>;

  uint64_t triples_{0};
  Counts by_s_;
  Counts by_p_;
  Counts by_o_;
  PairCounts by_sp_;
  PairCounts by_po_;
  PairCounts by_so_;
  std::unordered_set<Triple, TripleHash> all_;
};

}  // namespace ontodb
