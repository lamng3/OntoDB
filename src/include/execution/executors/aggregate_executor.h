#pragma once

#include "execution/executor_context.h"
#include "execution/executors/abstract_executor.h"
#include "execution/plans/algebra_plan.h"

namespace ontodb {

class AggregateExecutor : public AbstractExecutor {
 public:
  AggregateExecutor(ExecutorContext* ctx, std::unique_ptr<AbstractExecutor> child,
                    const AggregatePlan* plan);
  void Init() override;
  auto Next(Row* row) -> bool override;
  auto GetOutputSchema() const -> const Schema& override;

 private:
  ExecutorContext* ctx_;
  std::unique_ptr<AbstractExecutor> child_;
  const AggregatePlan* plan_;
  std::vector<Row> rows_;
  size_t at_{0};
  bool ready_{false};
};

}  // namespace ontodb
