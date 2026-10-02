#pragma once

#include "binder/bound_query.h"
#include "execution/plans/abstract_plan.h"

namespace ontodb {

// Joins triple patterns left to right in query order. FILTER sits above the
// joins. DISTINCT, LIMIT/OFFSET, and ORDER BY become plan nodes whose
// executors are phase 4.
class Planner {
 public:
  auto Plan(const BoundQuery& query) const -> PlanNodeRef;
};

}  // namespace ontodb
