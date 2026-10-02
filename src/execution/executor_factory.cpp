#include "execution/executor_factory.h"

#include "common/exception.h"
#include "execution/executors/filter_executor.h"
#include "execution/executors/nested_loop_join_executor.h"
#include "execution/executors/projection_executor.h"
#include "execution/executors/triple_scan_executor.h"
#include "execution/plans/filter_plan.h"
#include "execution/plans/nested_loop_join_plan.h"
#include "execution/plans/projection_plan.h"
#include "execution/plans/triple_scan_plan.h"

namespace ontodb {

auto ExecutorFactory::Create(const AbstractPlanNode* plan,
                             ExecutorContext* ctx) -> std::unique_ptr<AbstractExecutor> {
  switch (plan->GetType()) {
    case PlanType::kTripleScan:
      return std::make_unique<TripleScanExecutor>(ctx, static_cast<const TripleScanPlan*>(plan));
    case PlanType::kNestedLoopJoin: {
      auto left = Create(plan->GetChildAt(0), ctx);
      auto right = Create(plan->GetChildAt(1), ctx);
      return std::make_unique<NestedLoopJoinExecutor>(std::move(left), std::move(right),
                                                      static_cast<const NestedLoopJoinPlan*>(plan));
    }
    case PlanType::kFilter: {
      auto child = Create(plan->GetChildAt(0), ctx);
      return std::make_unique<FilterExecutor>(ctx, std::move(child),
                                              static_cast<const FilterPlan*>(plan));
    }
    case PlanType::kProjection: {
      auto child = Create(plan->GetChildAt(0), ctx);
      return std::make_unique<ProjectionExecutor>(std::move(child),
                                                  static_cast<const ProjectionPlan*>(plan));
    }
    case PlanType::kIndexNestedLoopJoin:
      throw NotImplementedException("PLAN 4.2: IndexNestedLoopJoinExecutor");
    case PlanType::kHashJoin:
      throw NotImplementedException("PLAN 4.3: HashJoinExecutor");
    case PlanType::kDistinct:
      throw NotImplementedException("PLAN 4.4: DistinctExecutor");
    case PlanType::kLimit:
      throw NotImplementedException("PLAN 4.4: LimitExecutor");
    case PlanType::kSort:
      throw NotImplementedException("PLAN 4.5: SortExecutor");
    case PlanType::kInsert:
      throw NotImplementedException("PLAN 4.6: InsertExecutor");
    case PlanType::kDelete:
      throw NotImplementedException("PLAN 4.6: DeleteExecutor");
  }
  throw NotImplementedException("PLAN 4.1: ExecutorFactory::Create");
}

}  // namespace ontodb
