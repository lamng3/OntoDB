#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

#include "execution/executor_context.h"
#include "execution/executors/abstract_executor.h"
#include "execution/plans/abstract_plan.h"

namespace ontodb {

// Rows each plan node produced, filled by executors the factory wraps for
// EXPLAIN ANALYZE.
using RowCounts = std::unordered_map<const AbstractPlanNode*, uint64_t>;

class ExecutorFactory {
 public:
  // With `counts`, every executor created is wrapped in one that adds each row
  // it returns to counts[its plan node].
  static auto Create(const AbstractPlanNode* plan, ExecutorContext* ctx,
                     RowCounts* counts = nullptr) -> std::unique_ptr<AbstractExecutor>;

 private:
  static auto Build(const AbstractPlanNode* plan, ExecutorContext* ctx,
                    RowCounts* counts) -> std::unique_ptr<AbstractExecutor>;
};

}  // namespace ontodb
