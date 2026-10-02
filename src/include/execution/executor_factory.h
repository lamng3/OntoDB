#pragma once

#include <memory>

#include "execution/executor_context.h"
#include "execution/executors/abstract_executor.h"
#include "execution/plans/abstract_plan.h"

namespace ontodb {

class ExecutorFactory {
 public:
  static auto Create(const AbstractPlanNode* plan,
                     ExecutorContext* ctx) -> std::unique_ptr<AbstractExecutor>;
};

}  // namespace ontodb
