#pragma once

#include "common/types.h"
#include "execution/plans/abstract_plan.h"
#include "optimizer/cardinality_estimator.h"

namespace ontodb {

// Rewrites a left-deep nested-loop plan. Join order and algorithm are chosen
// here. Config::join forces the algorithm when it is not kAuto: kNestedLoop
// stays a nested loop, kHash becomes HashJoin, kIndexNestedLoop becomes
// IndexNestedLoopJoin. kAuto may pick any of the three. The rewritten plan
// returns the same rows as the input.
class Optimizer {
 public:
  Optimizer(const CardinalityEstimator* estimator, const Config* config);
  auto Optimize(PlanNodeRef plan) -> PlanNodeRef;

  // Replaces every nested-loop join with the operator `method` names:
  // kHash -> HashJoin; kIndexNestedLoop -> IndexNestedLoopJoin where the inner
  // side is a single triple scan (other joins stay nested loops). kNestedLoop
  // and kAuto leave the plan as it is. Same rows; only the operator changes.
  static auto ApplyJoinMethod(PlanNodeRef plan, JoinMethod method) -> PlanNodeRef;

 private:
  const CardinalityEstimator* estimator_;
  const Config* config_;
};

}  // namespace ontodb
