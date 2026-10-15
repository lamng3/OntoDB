#pragma once

#include "binder/bound_query.h"
#include "stats/statistics.h"

namespace ontodb {

// Estimates are finite and never negative. A pattern with every position
// unbound estimates TripleCount. Adding a bound position never increases the
// estimate. The formulas behind that are yours.
//
// A single pattern's estimate is exact: Statistics::PatternCount of its
// constants (0 if a constant is not in the dictionary). A variable repeated
// in one pattern (?x p ?x) is not counted as a filter, so that estimate is
// high. Joins follow System R: |L| * |R| / max(V(L, x), V(R, x)) for each
// shared variable x, where V is the number of distinct values x takes;
// EstimateDistinct gives V for one pattern, from per-predicate distinct
// subject and object counts. EstimateJoin(l, r, k) is the fallback when V is
// unknown: it takes each side's row count as its V, so one shared variable
// gives min(l, r), and each further shared variable divides by the number of
// distinct subjects.
class CardinalityEstimator {
 public:
  explicit CardinalityEstimator(const Statistics* stats);
  auto EstimateTriplePattern(const BoundTriplePattern& pattern) const -> double;
  auto EstimateJoin(double left_rows, double right_rows, size_t shared_vars) const -> double;
  // Distinct values the variable in `slot` takes over the pattern's matches.
  auto EstimateDistinct(const BoundTriplePattern& pattern, size_t slot) const -> double;
  // One shared variable with `left_distinct` and `right_distinct` values.
  auto EstimateJoinWith(double left_rows, double right_rows, double left_distinct,
                        double right_distinct) const -> double;

 private:
  const Statistics* stats_;
};

}  // namespace ontodb
