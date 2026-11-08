#pragma once

#include <memory>

#include "execution/executor_context.h"
#include "execution/executors/abstract_executor.h"
#include "execution/plans/algebra_plan.h"

namespace ontodb {

class UpdateExecutor : public AbstractExecutor {
 public:
  UpdateExecutor(ExecutorContext* ctx, std::unique_ptr<AbstractExecutor> child,
                 const UpdatePlan* plan);
  void Init() override;
  auto Next(Row* row) -> bool override;
  auto GetOutputSchema() const -> const Schema& override;

 private:
  ExecutorContext* ctx_;
  std::unique_ptr<AbstractExecutor> child_;
  const UpdatePlan* plan_;
  bool done_{false};
};

}  // namespace ontodb
