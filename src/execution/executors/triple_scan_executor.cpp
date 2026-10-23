#include "execution/executors/triple_scan_executor.h"

#include <optional>

#include "concurrency/lock_resources.h"
#include "concurrency/transaction.h"
#include "execution/entailed_scan.h"
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
  InitWith(Row{});
}

void TripleScanExecutor::InitWith(const Row& bindings) {
  ctx_->CheckDeadline();
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
  auto value = [&](const BoundTerm& term) -> std::optional<term_id_t> {
    if (!term.is_variable) {
      return term.id;
    }
    if (term.slot < bindings.size() && bindings[term.slot] != INVALID_TERM_ID) {
      return bindings[term.slot];
    }
    return std::nullopt;
  };
  TriplePattern store_pattern;
  store_pattern.subject = value(pattern.subject);
  store_pattern.predicate = value(pattern.predicate);
  store_pattern.object = value(pattern.object);
  Transaction* txn = ctx_->GetTransaction();
  if (txn != nullptr && txn->GetIsolationLevel() != IsolationLevel::kReadUncommitted) {
    LockManager* locks = ctx_->GetLockManager();
    locks->Lock(txn, DatabaseResource(), LockMode::kIs, ctx_->GetSession());
    const bool range = txn->GetIsolationLevel() == IsolationLevel::kRepeatableRead;
    locks->Lock(txn, TriplesResource(), range ? LockMode::kS : LockMode::kIs, ctx_->GetSession());
  }
  if (pattern.entailed && ctx_->GetEntailment() != nullptr) {
    iter_ = EntailedScan(ctx_, pattern, store_pattern.subject);
  } else {
    iter_ = ctx_->GetStore()->Scan(store_pattern);
  }
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
    if ((++scanned_ & 4095U) == 0) {
      ctx_->CheckDeadline();
    }
    Row candidate(plan_->RowWidth(), INVALID_TERM_ID);
    if (!BindPosition(pattern.subject, triple.subject, &candidate) ||
        !BindPosition(pattern.predicate, triple.predicate, &candidate) ||
        !BindPosition(pattern.object, triple.object, &candidate)) {
      continue;
    }
    // An entailed triple is not stored, so there is no triple to lock or to find again; the lock
    // on the whole triple set taken in InitWith covers what the scan reads.
    if (!pattern.entailed && !LockRow(triple)) {
      continue;
    }
    *row = std::move(candidate);
    return true;
  }
  return false;
}

auto TripleScanExecutor::LockRow(const Triple& triple) -> bool {
  Transaction* txn = ctx_->GetTransaction();
  if (txn == nullptr || txn->GetIsolationLevel() == IsolationLevel::kReadUncommitted) {
    return true;
  }
  bool waited = false;
  ctx_->GetLockManager()->Lock(txn, TripleResource(triple), LockMode::kS, ctx_->GetSession(),
                               &waited);
  if (!waited) {
    return true;
  }
  auto check =
      ctx_->GetStore()->Scan(TriplePattern{triple.subject, triple.predicate, triple.object});
  check->Init();
  Triple found;
  return check->Next(&found);
}

auto TripleScanExecutor::GetOutputSchema() const -> const Schema& {
  return plan_->OutputSchema();
}

}  // namespace ontodb
