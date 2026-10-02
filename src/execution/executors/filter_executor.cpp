#include "execution/executors/filter_executor.h"

#include "execution/term_compare.h"

namespace ontodb {

FilterExecutor::FilterExecutor(ExecutorContext* ctx, std::unique_ptr<AbstractExecutor> child,
                               const FilterPlan* plan)
    : ctx_(ctx), child_(std::move(child)), plan_(plan) {}

void FilterExecutor::Init() {
  child_->Init();
}

auto FilterExecutor::ValueOf(const BoundTerm& term, const Row& row) const -> term_id_t {
  if (!term.is_variable) {
    return term.id;
  }
  if (term.slot >= row.size()) {
    return INVALID_TERM_ID;
  }
  return row[term.slot];
}

auto FilterExecutor::Eval(const BoundFilter& filter, const Row& row) const -> bool {
  if (filter.op == BoundFilter::Op::kAnd) {
    return Eval(*filter.lhs, row) && Eval(*filter.rhs, row);
  }
  const term_id_t left = ValueOf(filter.left, row);
  const term_id_t right = ValueOf(filter.right, row);
  if (left == INVALID_TERM_ID || right == INVALID_TERM_ID) {
    return false;
  }
  return CompareTerms(left, right, filter.op, *ctx_->GetDictionary());
}

auto FilterExecutor::Next(Row* row) -> bool {
  Row child;
  while (child_->Next(&child)) {
    if (Eval(plan_->Predicate(), child)) {
      *row = std::move(child);
      return true;
    }
  }
  return false;
}

auto FilterExecutor::GetOutputSchema() const -> const Schema& {
  return plan_->OutputSchema();
}

}  // namespace ontodb
