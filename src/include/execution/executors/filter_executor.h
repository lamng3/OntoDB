#pragma once

#include <memory>

#include "execution/executor_context.h"
#include "execution/executors/abstract_executor.h"
#include "execution/plans/filter_plan.h"

namespace ontodb {

class FilterExecutor : public AbstractExecutor {
 public:
  FilterExecutor(ExecutorContext* ctx, std::unique_ptr<AbstractExecutor> child,
                 const FilterPlan* plan);

  void Init() override;
  auto Next(Row* row) -> bool override;
  auto GetOutputSchema() const -> const Schema& override;

 private:
  auto Eval(const BoundFilter& filter, const Row& row) const -> bool;
  auto ValueOf(const BoundTerm& term, const Row& row) const -> term_id_t;

  ExecutorContext* ctx_;
  std::unique_ptr<AbstractExecutor> child_;
  const FilterPlan* plan_;
};

}  // namespace ontodb
