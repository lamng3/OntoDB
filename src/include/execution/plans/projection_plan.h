#pragma once

#include "execution/plans/abstract_plan.h"

namespace ontodb {

class ProjectionPlan : public AbstractPlanNode {
 public:
  ProjectionPlan(Schema output, PlanNodeRef child, std::vector<size_t> slots)
      : AbstractPlanNode(std::move(output), {}), slots_(std::move(slots)) {
    children_.push_back(std::move(child));
  }

  auto GetType() const -> PlanType override { return PlanType::kProjection; }
  auto Label() const -> std::string override {
    std::string text = "Projection [";
    for (size_t i = 0; i < output_.columns.size(); i++) {
      if (i != 0) {
        text += ", ";
      }
      text += "?" + output_.columns[i].name;
    }
    text += "]";
    return text;
  }
  auto Slots() const -> const std::vector<size_t>& { return slots_; }

 private:
  std::vector<size_t> slots_;
};

}  // namespace ontodb
