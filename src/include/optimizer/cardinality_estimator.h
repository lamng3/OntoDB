#pragma once

#include "binder/bound_query.h"
#include "stats/statistics.h"

namespace ontodb {

// Estimates are finite and never negative. A pattern with every position
// unbound estimates TripleCount. Adding a bound position never increases the
// estimate. The formulas behind that are yours.
class CardinalityEstimator {
 public:
  explicit CardinalityEstimator(const Statistics* stats);
  auto EstimateTriplePattern(const BoundTriplePattern& pattern) const -> double;
  auto EstimateJoin(double left_rows, double right_rows, size_t shared_vars) const -> double;

 private:
  const Statistics* stats_;
};

}  // namespace ontodb
