#include "execution/executors/nested_loop_join_executor.h"

namespace ontodb {

NestedLoopJoinExecutor::NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left,
                                               std::unique_ptr<AbstractExecutor> right,
                                               const NestedLoopJoinPlan* plan)
    : left_(std::move(left)), right_(std::move(right)), plan_(plan) {}

void NestedLoopJoinExecutor::Init() {
  left_->Init();
  left_ok_ = left_->Next(&left_row_);
  if (left_ok_) {
    right_->Init();
  }
}

auto NestedLoopJoinExecutor::Merge(const Row& left, const Row& right, Row* out) const -> bool {
  const size_t width = plan_->RowWidth();
  out->assign(width, INVALID_TERM_ID);
  for (size_t i = 0; i < width; i++) {
    const bool left_set = i < left.size() && left[i] != INVALID_TERM_ID;
    const bool right_set = i < right.size() && right[i] != INVALID_TERM_ID;
    if (left_set && right_set && left[i] != right[i]) {
      return false;
    }
    (*out)[i] = left_set ? left[i] : (right_set ? right[i] : INVALID_TERM_ID);
  }
  return true;
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
      right_->Init();
    }
  }
  return false;
}

auto NestedLoopJoinExecutor::GetOutputSchema() const -> const Schema& {
  return plan_->OutputSchema();
}

}  // namespace ontodb
