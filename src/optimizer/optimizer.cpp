#include "optimizer/optimizer.h"

#include "common/exception.h"
#include "execution/plans/stub_plans.h"

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

auto Optimizer::ApplyJoinMethod(PlanNodeRef plan, JoinMethod method) -> PlanNodeRef {
  if (method != JoinMethod::kHash && method != JoinMethod::kIndexNestedLoop) {
    return plan;
  }
  auto children = plan->ReleaseChildren();
  for (auto& child : children) {
    child = ApplyJoinMethod(std::move(child), method);
  }
  if (plan->GetType() != PlanType::kNestedLoopJoin) {
    plan->SetChildren(std::move(children));
    return plan;
  }
  if (method == JoinMethod::kHash) {
    return std::make_unique<HashJoinPlan>(plan->OutputSchema(), std::move(children[0]),
                                          std::move(children[1]));
  }
  if (children[1]->GetType() == PlanType::kTripleScan) {
    return std::make_unique<IndexNestedLoopJoinPlan>(plan->OutputSchema(), std::move(children[0]),
                                                     std::move(children[1]));
  }
  plan->SetChildren(std::move(children));
  return plan;
}

auto Optimizer::Optimize(PlanNodeRef) -> PlanNodeRef {
  throw NotImplementedException("PLAN 5.3: Optimizer::Optimize");
}

}  // namespace ontodb
