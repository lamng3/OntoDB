#pragma once

#include "binder/bound_query.h"
#include "execution/plans/abstract_plan.h"

namespace ontodb {

class FilterPlan : public AbstractPlanNode {
 public:
  FilterPlan(Schema output, PlanNodeRef child, std::unique_ptr<BoundFilter> predicate)
      : AbstractPlanNode(std::move(output), {}), predicate_(std::move(predicate)) {
    children_.push_back(std::move(child));
  }

  auto GetType() const -> PlanType override { return PlanType::kFilter; }
  auto Label() const -> std::string override { return "Filter (" + predicate_->text + ")"; }
  auto Predicate() const -> const BoundFilter& { return *predicate_; }

 private:
  std::unique_ptr<BoundFilter> predicate_;
};

}  // namespace ontodb
