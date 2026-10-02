#include "optimizer/optimizer.h"

#include "common/exception.h"

namespace ontodb {

CardinalityEstimator::CardinalityEstimator(const Statistics* stats) : stats_(stats) {}

auto CardinalityEstimator::EstimateTriplePattern(const BoundTriplePattern&) const -> double {
  throw NotImplementedException("PLAN 5.2: CardinalityEstimator::EstimateTriplePattern");
}
auto CardinalityEstimator::EstimateJoin(double, double, size_t) const -> double {
  throw NotImplementedException("PLAN 5.2: CardinalityEstimator::EstimateJoin");
}

Optimizer::Optimizer(const CardinalityEstimator* estimator, const Config* config)
    : estimator_(estimator), config_(config) {}

auto Optimizer::Optimize(PlanNodeRef) -> PlanNodeRef {
  throw NotImplementedException("PLAN 5.3: Optimizer::Optimize");
}

}  // namespace ontodb
