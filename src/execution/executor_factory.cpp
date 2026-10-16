#include "execution/executor_factory.h"

#include "common/exception.h"
#include "execution/executors/distinct_limit_executor.h"
#include "execution/executors/filter_executor.h"
#include "execution/executors/hash_join_executor.h"
#include "execution/executors/index_nested_loop_join_executor.h"
#include "execution/executors/modify_executor.h"
#include "execution/executors/nested_loop_join_executor.h"
#include "execution/executors/projection_executor.h"
#include "execution/executors/sort_executor.h"
#include "execution/executors/triple_scan_executor.h"
#include "execution/plans/filter_plan.h"
#include "execution/plans/nested_loop_join_plan.h"
#include "execution/plans/projection_plan.h"
#include "execution/plans/triple_scan_plan.h"

namespace ontodb {

namespace {

// Counts the rows another executor returns, for EXPLAIN ANALYZE.
class CountingExecutor : public AbstractExecutor {
 public:
  CountingExecutor(std::unique_ptr<AbstractExecutor> inner, uint64_t* count)
      : inner_(std::move(inner)), count_(count) {}
  void Init() override { inner_->Init(); }
  void InitWith(const Row& bindings) override { inner_->InitWith(bindings); }
  auto Next(Row* row) -> bool override {
    const bool ok = inner_->Next(row);
    *count_ += ok ? 1 : 0;
    return ok;
  }
  auto GetOutputSchema() const -> const Schema& override { return inner_->GetOutputSchema(); }

 private:
  std::unique_ptr<AbstractExecutor> inner_;
  uint64_t* count_;
};

}  // namespace

auto ExecutorFactory::Create(const AbstractPlanNode* plan, ExecutorContext* ctx,
                             RowCounts* counts) -> std::unique_ptr<AbstractExecutor> {
  auto executor = Build(plan, ctx, counts);
  if (counts == nullptr) {
    return executor;
  }
  return std::make_unique<CountingExecutor>(std::move(executor), &(*counts)[plan]);
}

auto ExecutorFactory::Build(const AbstractPlanNode* plan, ExecutorContext* ctx,
                            RowCounts* counts) -> std::unique_ptr<AbstractExecutor> {
  switch (plan->GetType()) {
    case PlanType::kTripleScan:
      return std::make_unique<TripleScanExecutor>(ctx, static_cast<const TripleScanPlan*>(plan));
    case PlanType::kNestedLoopJoin: {
      auto left = Create(plan->GetChildAt(0), ctx, counts);
      auto right = Create(plan->GetChildAt(1), ctx, counts);
      return std::make_unique<NestedLoopJoinExecutor>(std::move(left), std::move(right),
                                                      static_cast<const NestedLoopJoinPlan*>(plan));
    }
    case PlanType::kFilter: {
      auto child = Create(plan->GetChildAt(0), ctx, counts);
      return std::make_unique<FilterExecutor>(ctx, std::move(child),
                                              static_cast<const FilterPlan*>(plan));
    }
    case PlanType::kProjection: {
      auto child = Create(plan->GetChildAt(0), ctx, counts);
      return std::make_unique<ProjectionExecutor>(std::move(child),
                                                  static_cast<const ProjectionPlan*>(plan));
    }
    case PlanType::kIndexNestedLoopJoin:
      return std::make_unique<IndexNestedLoopJoinExecutor>(
          ctx, Create(plan->GetChildAt(0), ctx, counts),
          static_cast<const IndexNestedLoopJoinPlan*>(plan));
    case PlanType::kHashJoin:
      return std::make_unique<HashJoinExecutor>(Create(plan->GetChildAt(0), ctx, counts),
                                                Create(plan->GetChildAt(1), ctx, counts),
                                                static_cast<const HashJoinPlan*>(plan));
    case PlanType::kDistinct:
      return std::make_unique<DistinctExecutor>(Create(plan->GetChildAt(0), ctx, counts),
                                                static_cast<const DistinctPlan*>(plan));
    case PlanType::kLimit:
      return std::make_unique<LimitExecutor>(Create(plan->GetChildAt(0), ctx, counts),
                                             static_cast<const LimitPlan*>(plan));
    case PlanType::kSort:
      return std::make_unique<SortExecutor>(ctx, Create(plan->GetChildAt(0), ctx, counts),
                                            static_cast<const SortPlan*>(plan));
    case PlanType::kInsert:
      return std::make_unique<ModifyExecutor>(ctx, plan, true);
    case PlanType::kDelete:
      return std::make_unique<ModifyExecutor>(ctx, plan, false);
  }
  throw NotImplementedException("PLAN 4.1: ExecutorFactory::Create");
}

}  // namespace ontodb
