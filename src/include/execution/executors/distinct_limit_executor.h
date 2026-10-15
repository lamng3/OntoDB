#pragma once

#include <memory>
#include <unordered_set>

#include "execution/executors/abstract_executor.h"
#include "execution/plans/stub_plans.h"
#include "execution/row_merge.h"

namespace ontodb {

// Passes each row the first time it appears, in the child's order. It sits
// above the projection, so "the same row" means the same projected values.
// It remembers every distinct row it has passed.
class DistinctExecutor : public AbstractExecutor {
 public:
  DistinctExecutor(std::unique_ptr<AbstractExecutor> child, const DistinctPlan* plan);
  void Init() override;
  auto Next(Row* row) -> bool override;
  auto GetOutputSchema() const -> const Schema& override;

 private:
  std::unique_ptr<AbstractExecutor> child_;
  const DistinctPlan* plan_;
  std::unordered_set<Row, RowHash> seen_;
};

// Skips OFFSET rows, then passes at most LIMIT rows, in the child's order.
// Once the limit is reached it stops asking the child, so the rest of the
// plan does no more work.
class LimitExecutor : public AbstractExecutor {
 public:
  LimitExecutor(std::unique_ptr<AbstractExecutor> child, const LimitPlan* plan);
  void Init() override;
  auto Next(Row* row) -> bool override;
  auto GetOutputSchema() const -> const Schema& override;

 private:
  std::unique_ptr<AbstractExecutor> child_;
  const LimitPlan* plan_;
  uint64_t skipped_{0};
  uint64_t passed_{0};
};

}  // namespace ontodb
