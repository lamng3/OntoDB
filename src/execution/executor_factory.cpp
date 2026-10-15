#include "execution/executor_factory.h"

#include "common/exception.h"
#include "execution/executors/distinct_limit_executor.h"
#include "execution/executors/filter_executor.h"
#include "execution/executors/hash_join_executor.h"
#include "execution/executors/index_nested_loop_join_executor.h"
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
      return std::make_unique<IndexNestedLoopJoinExecutor>(
          ctx, Create(plan->GetChildAt(0), ctx), static_cast<const IndexNestedLoopJoinPlan*>(plan));
    case PlanType::kHashJoin:
      return std::make_unique<HashJoinExecutor>(Create(plan->GetChildAt(0), ctx),
                                                Create(plan->GetChildAt(1), ctx),
                                                static_cast<const HashJoinPlan*>(plan));
    case PlanType::kDistinct:
      return std::make_unique<DistinctExecutor>(Create(plan->GetChildAt(0), ctx),
                                                static_cast<const DistinctPlan*>(plan));
    case PlanType::kLimit:
      return std::make_unique<LimitExecutor>(Create(plan->GetChildAt(0), ctx),
                                             static_cast<const LimitPlan*>(plan));
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
