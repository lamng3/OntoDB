#pragma once

#include <vector>

#include "execution/executor_context.h"
#include "execution/executors/abstract_executor.h"
#include "execution/plans/stub_plans.h"

namespace ontodb {

// INSERT DATA and DELETE DATA. The binder has already put every term in the
// dictionary, so the plan holds ids; the executor writes the store, which
// updates every index or none for each triple. A triple already present (for
// insert) or absent (for delete) changes nothing. If a write throws, the
// triples this statement already changed are put back before the exception
// propagates, so the statement is atomic on either backend. Next returns one
// row holding the number of triples changed, then false.
//
// Under a transaction it takes IX on the database and on the triple set, then
// X on each triple before writing it, and records each change in the
// transaction's write set (taken back out again if the statement undoes it).
// Exclusive locks are held to the end of the transaction at every level.
class ModifyExecutor : public AbstractExecutor {
 public:
  ModifyExecutor(ExecutorContext* ctx, const AbstractPlanNode* plan, bool insert);

  void Init() override;
  auto Next(Row* row) -> bool override;
  auto GetOutputSchema() const -> const Schema& override;

 private:
  ExecutorContext* ctx_;
  const AbstractPlanNode* plan_;
  const std::vector<Triple>* triples_;
  bool insert_;
  bool done_{false};
};

}  // namespace ontodb
