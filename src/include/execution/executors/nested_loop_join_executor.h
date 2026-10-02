#pragma once

#include <memory>

#include "execution/executor_context.h"
#include "execution/executors/abstract_executor.h"
#include "execution/plans/nested_loop_join_plan.h"

namespace ontodb {

// Nested loop over two scans. The inner scan is not rebound with outer
// values; that is PLAN 4.1. Shared variables must be equal or the pair is
// skipped.
class NestedLoopJoinExecutor : public AbstractExecutor {
 public:
  NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left,
                         std::unique_ptr<AbstractExecutor> right, const NestedLoopJoinPlan* plan);

  void Init() override;
  auto Next(Row* row) -> bool override;
  auto GetOutputSchema() const -> const Schema& override;

 private:
  auto Merge(const Row& left, const Row& right, Row* out) const -> bool;

  std::unique_ptr<AbstractExecutor> left_;
  std::unique_ptr<AbstractExecutor> right_;
  const NestedLoopJoinPlan* plan_;
  Row left_row_;
  bool left_ok_{false};
};

}  // namespace ontodb
