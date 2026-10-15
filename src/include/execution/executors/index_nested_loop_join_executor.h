#pragma once

#include <memory>

#include "execution/executor_context.h"
#include "execution/executors/abstract_executor.h"
#include "execution/plans/stub_plans.h"
#include "execution/plans/triple_scan_plan.h"
#include "store/triple_iterator.h"

namespace ontodb {

// For each outer row, probes the store with the inner triple pattern, every
// variable the outer row binds filled in as a constant, and extends the outer
// row with each match. The inner child of the plan must be a TripleScanPlan;
// there is no inner executor. On an indexed store the probe is a prefix range
// of the index whose order begins with the bound positions.
class IndexNestedLoopJoinExecutor : public AbstractExecutor {
 public:
  IndexNestedLoopJoinExecutor(ExecutorContext* ctx, std::unique_ptr<AbstractExecutor> left,
                              const IndexNestedLoopJoinPlan* plan);

  void Init() override;
  auto Next(Row* row) -> bool override;
  auto GetOutputSchema() const -> const Schema& override;

 private:
  auto Probe() -> bool;  // opens the inner probe for left_row_; false when left is exhausted

  ExecutorContext* ctx_;
  std::unique_ptr<AbstractExecutor> left_;
  const IndexNestedLoopJoinPlan* plan_;
  const TripleScanPlan* inner_;
  size_t width_;
  Row left_row_;
  std::unique_ptr<TripleIterator> probe_;
};

}  // namespace ontodb
