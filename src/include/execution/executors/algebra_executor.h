#pragma once

#include <memory>

#include "execution/executor_context.h"
#include "execution/executors/abstract_executor.h"
#include "execution/plans/algebra_plan.h"

namespace ontodb {

// Evaluates a group graph pattern that is not a flat basic graph pattern.
class AlgebraExecutor : public AbstractExecutor {
 public:
  AlgebraExecutor(ExecutorContext* ctx, const AlgebraPlan* plan);
  void Init() override;
  auto Next(Row* row) -> bool override;
  auto GetOutputSchema() const -> const Schema& override;

 private:
  ExecutorContext* ctx_;
  const AlgebraPlan* plan_;
  std::vector<Row> rows_;
  size_t at_{0};
  bool ready_{false};
};

}  // namespace ontodb
