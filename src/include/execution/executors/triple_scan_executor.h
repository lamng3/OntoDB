#pragma once

#include <memory>

#include "execution/executor_context.h"
#include "execution/executors/abstract_executor.h"
#include "execution/plans/triple_scan_plan.h"
#include "store/triple_iterator.h"

namespace ontodb {

class TripleScanExecutor : public AbstractExecutor {
 public:
  TripleScanExecutor(ExecutorContext* ctx, const TripleScanPlan* plan);

  void Init() override;
  auto Next(Row* row) -> bool override;
  auto GetOutputSchema() const -> const Schema& override;

 private:
  auto BindPosition(const BoundTerm& term, term_id_t value, Row* row) const -> bool;
  auto Vacuous() const -> bool;

  ExecutorContext* ctx_;
  const TripleScanPlan* plan_;
  std::unique_ptr<TripleIterator> iter_;
  bool yielded_vacuous_{false};
};

}  // namespace ontodb
