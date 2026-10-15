#pragma once

#include <vector>

#include "common/types.h"
#include "execution/plans/abstract_plan.h"
#include "optimizer/cardinality_estimator.h"

namespace ontodb {

// Rewrites a left-deep nested-loop plan. Join order and algorithm are chosen
// here. Config::join forces the algorithm when it is not kAuto: kNestedLoop
// stays a nested loop, kHash becomes HashJoin, kIndexNestedLoop becomes
// IndexNestedLoopJoin. kAuto may pick any of the three. The rewritten plan
// returns the same rows as the input.
//
// Join order (PLAN 5.3) is greedy and left-deep: each nested-loop chain is
// flattened into its triple patterns, then rebuilt starting from the pattern
// with the fewest estimated rows and adding, at each step, the connected
// pattern (one sharing a variable with what is already joined) that gives the
// smallest estimated join; a cross product is taken only when nothing is
// connected. Ties keep the textual order. Estimates carry each variable's
// distinct-value count through the chain (CardinalityEstimator).
//
// With kAuto (PLAN 5.4), each join of the rebuilt chain picks its operator:
// a cross product stays a nested loop; otherwise an index nested-loop join
// when the estimated outer side is no larger than the inner pattern (few
// probes), and a hash join, built on the smaller inner pattern, when it is.
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
  auto Reorder(PlanNodeRef plan) -> PlanNodeRef;

  const CardinalityEstimator* estimator_;
  const Config* config_;
};

}  // namespace ontodb
