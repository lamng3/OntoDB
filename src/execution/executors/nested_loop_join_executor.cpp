#include "execution/executors/nested_loop_join_executor.h"

#include "execution/row_merge.h"

namespace ontodb {

NestedLoopJoinExecutor::NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left,
                                               std::unique_ptr<AbstractExecutor> right,
                                               const NestedLoopJoinPlan* plan)
    : left_(std::move(left)), right_(std::move(right)), plan_(plan) {}

void NestedLoopJoinExecutor::Init() {
  left_->Init();
  left_ok_ = left_->Next(&left_row_);
  if (left_ok_) {
    right_->InitWith(left_row_);
  }
}

auto NestedLoopJoinExecutor::Merge(const Row& left, const Row& right, Row* out) const -> bool {
  return MergeRows(left, right, plan_->RowWidth(), out);
}

auto NestedLoopJoinExecutor::Next(Row* row) -> bool {
  while (left_ok_) {
    Row right_row;
    while (right_->Next(&right_row)) {
      if (Merge(left_row_, right_row, row)) {
        return true;
      }
    }
    left_ok_ = left_->Next(&left_row_);
    if (left_ok_) {
      right_->InitWith(left_row_);
    }
  }
  return false;
}

auto NestedLoopJoinExecutor::GetOutputSchema() const -> const Schema& {
  return plan_->OutputSchema();
}

}  // namespace ontodb
