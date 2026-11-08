#pragma once

#include <memory>
#include <vector>

#include "binder/bound_query.h"
#include "execution/plans/abstract_plan.h"

namespace ontodb {

class AlgebraPlan : public AbstractPlanNode {
 public:
  AlgebraPlan(Schema output, std::shared_ptr<BoundPattern> pattern, size_t width)
      : AbstractPlanNode(std::move(output), {}), pattern_(std::move(pattern)), width_(width) {}
  auto GetType() const -> PlanType override { return PlanType::kAlgebra; }
  auto Label() const -> std::string override { return "Algebra"; }
  auto Pattern() const -> const BoundPattern& { return *pattern_; }
  auto Width() const -> size_t { return width_; }

 private:
  std::shared_ptr<BoundPattern> pattern_;
  size_t width_;
};

class AggregatePlan : public AbstractPlanNode {
 public:
  AggregatePlan(Schema output, PlanNodeRef child, std::vector<BoundAggregate> aggregates,
                std::vector<size_t> group_slots, std::unique_ptr<BoundFilter> having)
      : AbstractPlanNode(std::move(output), {})
      , aggregates_(std::move(aggregates))
      , group_slots_(std::move(group_slots))
      , having_(std::move(having)) {
    children_.push_back(std::move(child));
  }
  auto GetType() const -> PlanType override { return PlanType::kAggregate; }
  auto Label() const -> std::string override { return "Aggregate"; }
  auto Aggregates() const -> const std::vector<BoundAggregate>& { return aggregates_; }
  auto GroupSlots() const -> const std::vector<size_t>& { return group_slots_; }
  auto Having() const -> const BoundFilter* { return having_.get(); }

 private:
  std::vector<BoundAggregate> aggregates_;
  std::vector<size_t> group_slots_;
  std::unique_ptr<BoundFilter> having_;
};

class UpdatePlan : public AbstractPlanNode {
 public:
  UpdatePlan(Schema output, PlanNodeRef child, std::vector<BoundTriplePattern> deletes,
             std::vector<BoundTriplePattern> inserts, size_t width)
      : AbstractPlanNode(std::move(output), {})
      , deletes_(std::move(deletes))
      , inserts_(std::move(inserts))
      , width_(width) {
    children_.push_back(std::move(child));
  }
  auto GetType() const -> PlanType override { return PlanType::kUpdate; }
  auto Label() const -> std::string override { return "Update"; }
  auto Deletes() const -> const std::vector<BoundTriplePattern>& { return deletes_; }
  auto Inserts() const -> const std::vector<BoundTriplePattern>& { return inserts_; }
  auto Width() const -> size_t { return width_; }

 private:
  std::vector<BoundTriplePattern> deletes_;
  std::vector<BoundTriplePattern> inserts_;
  size_t width_;
};

}  // namespace ontodb
