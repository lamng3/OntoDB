#include "execution/executors/distinct_limit_executor.h"

namespace ontodb {

DistinctExecutor::DistinctExecutor(std::unique_ptr<AbstractExecutor> child,
                                   const DistinctPlan* plan)
    : child_(std::move(child)), plan_(plan) {}

void DistinctExecutor::Init() {
  seen_.clear();
  child_->Init();
}

auto DistinctExecutor::Next(Row* row) -> bool {
  while (child_->Next(row)) {
    if (seen_.insert(*row).second) {
      return true;
    }
  }
  return false;
}

auto DistinctExecutor::GetOutputSchema() const -> const Schema& {
  return plan_->OutputSchema();
}

LimitExecutor::LimitExecutor(std::unique_ptr<AbstractExecutor> child, const LimitPlan* plan)
    : child_(std::move(child)), plan_(plan) {}

void LimitExecutor::Init() {
  skipped_ = 0;
  passed_ = 0;
  child_->Init();
}

auto LimitExecutor::Next(Row* row) -> bool {
  const uint64_t offset = plan_->Offset().value_or(0);
  while (skipped_ < offset) {
    if (!child_->Next(row)) {
      return false;
    }
    skipped_++;
  }
  const auto limit = plan_->Limit();
  if (limit.has_value() && passed_ >= *limit) {
    return false;
  }
  if (!child_->Next(row)) {
    return false;
  }
  passed_++;
  return true;
}

auto LimitExecutor::GetOutputSchema() const -> const Schema& {
  return plan_->OutputSchema();
}

}  // namespace ontodb
