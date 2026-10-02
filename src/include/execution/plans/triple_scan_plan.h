#pragma once

#include "binder/bound_query.h"
#include "execution/plans/abstract_plan.h"

namespace ontodb {

class TripleScanPlan : public AbstractPlanNode {
 public:
  TripleScanPlan(Schema output, BoundTriplePattern pattern, size_t row_width)
      : AbstractPlanNode(std::move(output), {})
      , pattern_(std::move(pattern))
      , row_width_(row_width) {}

  auto GetType() const -> PlanType override { return PlanType::kTripleScan; }
  auto Label() const -> std::string override { return "TripleScan {" + pattern_.text + "}"; }
  auto Pattern() const -> const BoundTriplePattern& { return pattern_; }
  auto RowWidth() const -> size_t { return row_width_; }

 private:
  BoundTriplePattern pattern_;
  size_t row_width_;
};

}  // namespace ontodb
