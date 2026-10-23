#include "execution/executors/index_nested_loop_join_executor.h"

#include <algorithm>
#include <optional>
#include <stdexcept>

#include "execution/entailed_scan.h"
#include "store/triple_store.h"

namespace ontodb {

IndexNestedLoopJoinExecutor::IndexNestedLoopJoinExecutor(ExecutorContext* ctx,
                                                         std::unique_ptr<AbstractExecutor> left,
                                                         const IndexNestedLoopJoinPlan* plan)
    : ctx_(ctx), left_(std::move(left)), plan_(plan) {
  if (plan_->GetChildAt(1)->GetType() != PlanType::kTripleScan) {
    throw std::logic_error("an index nested-loop join needs a triple scan as its inner side");
  }
  inner_ = static_cast<const TripleScanPlan*>(plan_->GetChildAt(1));
  width_ = std::max(plan_->OutputSchema().columns.size(), inner_->RowWidth());
}

void IndexNestedLoopJoinExecutor::Init() {
  probe_.reset();
  left_->Init();
}

auto IndexNestedLoopJoinExecutor::Probe() -> bool {
  ctx_->CheckDeadline();
  probe_.reset();
  if (!left_->Next(&left_row_)) {
    return false;
  }
  left_row_.resize(width_, INVALID_TERM_ID);
  const auto& pattern = inner_->Pattern();
  auto value = [&](const BoundTerm& term) -> std::optional<term_id_t> {
    if (!term.is_variable) {
      return term.id;
    }
    if (left_row_[term.slot] != INVALID_TERM_ID) {
      return left_row_[term.slot];
    }
    return std::nullopt;
  };
  if (pattern.entailed && ctx_->GetEntailment() != nullptr) {
    probe_ = EntailedScan(ctx_, pattern, value(pattern.subject));
  } else {
    probe_ = ctx_->GetStore()->Scan(
        TriplePattern{value(pattern.subject), value(pattern.predicate), value(pattern.object)});
  }
  probe_->Init();
  return true;
}

auto IndexNestedLoopJoinExecutor::Next(Row* row) -> bool {
  const auto& pattern = inner_->Pattern();
  while (true) {
    if (probe_ == nullptr && !Probe()) {
      return false;
    }
    Triple triple;
    while (probe_->Next(&triple)) {
      Row out = left_row_;
      bool ok = true;
      for (const auto& [term, value] : {std::pair{&pattern.subject, triple.subject},
                                        std::pair{&pattern.predicate, triple.predicate},
                                        std::pair{&pattern.object, triple.object}}) {
        if (!term->is_variable) {
          ok = ok && term->id == value;
        } else if (out[term->slot] == INVALID_TERM_ID) {
          out[term->slot] = value;
        } else {
          ok = ok && out[term->slot] == value;  // e.g. ?x knows ?x
        }
      }
      if (ok) {
        *row = std::move(out);
        return true;
      }
    }
    probe_.reset();
  }
}

auto IndexNestedLoopJoinExecutor::GetOutputSchema() const -> const Schema& {
  return plan_->OutputSchema();
}

}  // namespace ontodb
