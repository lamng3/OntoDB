#include "execution/executors/triple_scan_executor.h"

#include "store/triple_store.h"

namespace ontodb {

TripleScanExecutor::TripleScanExecutor(ExecutorContext* ctx, const TripleScanPlan* plan)
    : ctx_(ctx), plan_(plan) {}

auto TripleScanExecutor::Vacuous() const -> bool {
  const auto& pattern = plan_->Pattern();
  auto empty = [](const BoundTerm& term) { return !term.is_variable && term.text.empty(); };
  return empty(pattern.subject) && empty(pattern.predicate) && empty(pattern.object);
}

auto TripleScanExecutor::BindPosition(const BoundTerm& term, term_id_t value,
                                      Row* row) const -> bool {
  if (!term.is_variable) {
    return value == term.id;
  }
  if ((*row)[term.slot] == INVALID_TERM_ID) {
    (*row)[term.slot] = value;
    return true;
  }
  return (*row)[term.slot] == value;
}

void TripleScanExecutor::Init() {
  yielded_vacuous_ = false;
  iter_.reset();
  if (Vacuous()) {
    return;
  }
  const auto& pattern = plan_->Pattern();
  for (const BoundTerm* term : {&pattern.subject, &pattern.predicate, &pattern.object}) {
    if (!term->is_variable && term->id == INVALID_TERM_ID) {
      return;
    }
  }
  TriplePattern store_pattern;
  if (!pattern.subject.is_variable) {
    store_pattern.subject = pattern.subject.id;
  }
  if (!pattern.predicate.is_variable) {
    store_pattern.predicate = pattern.predicate.id;
  }
  if (!pattern.object.is_variable) {
    store_pattern.object = pattern.object.id;
  }
  iter_ = ctx_->GetStore()->Scan(store_pattern);
  iter_->Init();
}

auto TripleScanExecutor::Next(Row* row) -> bool {
  if (Vacuous()) {
    if (yielded_vacuous_) {
      return false;
    }
    yielded_vacuous_ = true;
    *row = Row(plan_->RowWidth(), INVALID_TERM_ID);
    return true;
  }
  if (iter_ == nullptr) {
    return false;
  }
  const auto& pattern = plan_->Pattern();
  Triple triple;
  while (iter_->Next(&triple)) {
    Row candidate(plan_->RowWidth(), INVALID_TERM_ID);
    if (!BindPosition(pattern.subject, triple.subject, &candidate) ||
        !BindPosition(pattern.predicate, triple.predicate, &candidate) ||
        !BindPosition(pattern.object, triple.object, &candidate)) {
      continue;
    }
    *row = std::move(candidate);
    return true;
  }
  return false;
}

auto TripleScanExecutor::GetOutputSchema() const -> const Schema& {
  return plan_->OutputSchema();
}

}  // namespace ontodb
