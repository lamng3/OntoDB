#include "execution/executors/projection_executor.h"

namespace ontodb {

ProjectionExecutor::ProjectionExecutor(std::unique_ptr<AbstractExecutor> child,
                                       const ProjectionPlan* plan)
    : child_(std::move(child)), plan_(plan) {}

void ProjectionExecutor::Init() {
  child_->Init();
}

auto ProjectionExecutor::Next(Row* row) -> bool {
  Row child;
  if (!child_->Next(&child)) {
    return false;
  }
  row->clear();
  row->reserve(plan_->Slots().size());
  for (const size_t slot : plan_->Slots()) {
    row->push_back(slot < child.size() ? child[slot] : INVALID_TERM_ID);
  }
  return true;
}

auto ProjectionExecutor::GetOutputSchema() const -> const Schema& {
  return plan_->OutputSchema();
}

}  // namespace ontodb
