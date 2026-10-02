#pragma once

#include "execution/plans/abstract_plan.h"

namespace ontodb {

class NestedLoopJoinPlan : public AbstractPlanNode {
 public:
  NestedLoopJoinPlan(Schema output, PlanNodeRef left, PlanNodeRef right, size_t row_width)
      : AbstractPlanNode(std::move(output), {}), row_width_(row_width) {
    children_.push_back(std::move(left));
    children_.push_back(std::move(right));
  }

  auto GetType() const -> PlanType override { return PlanType::kNestedLoopJoin; }
  auto Label() const -> std::string override { return "NestedLoopJoin"; }
  auto RowWidth() const -> size_t { return row_width_; }

 private:
  size_t row_width_;
};

}  // namespace ontodb
