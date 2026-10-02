#include "planner/planner.h"

#include "execution/plans/filter_plan.h"
#include "execution/plans/nested_loop_join_plan.h"
#include "execution/plans/projection_plan.h"
#include "execution/plans/stub_plans.h"
#include "execution/plans/triple_scan_plan.h"

namespace ontodb {
namespace {

auto FullSchema(const BoundQuery& query) -> Schema {
  Schema schema;
  for (size_t i = 0; i < query.var_names.size(); i++) {
    schema.columns.push_back(Column{query.var_names[i], static_cast<uint32_t>(i)});
  }
  return schema;
}

auto CloneFilter(const BoundFilter& filter) -> std::unique_ptr<BoundFilter> {
  auto copy = std::make_unique<BoundFilter>();
  copy->op = filter.op;
  copy->left = filter.left;
  copy->right = filter.right;
  copy->text = filter.text;
  if (filter.lhs != nullptr) {
    copy->lhs = CloneFilter(*filter.lhs);
  }
  if (filter.rhs != nullptr) {
    copy->rhs = CloneFilter(*filter.rhs);
  }
  return copy;
}

}  // namespace

auto Planner::Plan(const BoundQuery& query) const -> PlanNodeRef {
  if (query.kind == BoundQuery::Kind::kInsert) {
    return std::make_unique<InsertPlan>(query.ground_triples);
  }
  if (query.kind == BoundQuery::Kind::kDelete) {
    return std::make_unique<DeletePlan>(query.ground_triples);
  }

  const Schema wide = FullSchema(query);
  PlanNodeRef plan;
  for (const auto& pattern : query.patterns) {
    auto scan = std::make_unique<TripleScanPlan>(wide, pattern, query.var_names.size());
    if (plan == nullptr) {
      plan = std::move(scan);
    } else {
      plan = std::make_unique<NestedLoopJoinPlan>(wide, std::move(plan), std::move(scan),
                                                  query.var_names.size());
    }
  }
  if (plan == nullptr) {
    plan = std::make_unique<TripleScanPlan>(wide, BoundTriplePattern{}, query.var_names.size());
  }
  if (query.filter != nullptr) {
    plan = std::make_unique<FilterPlan>(wide, std::move(plan), CloneFilter(*query.filter));
  }

  Schema projected;
  for (size_t i = 0; i < query.projection_names.size(); i++) {
    projected.columns.push_back(Column{query.projection_names[i], static_cast<uint32_t>(i)});
  }
  if (!query.order_by.empty()) {
    plan = std::make_unique<SortPlan>(wide, std::move(plan), query.order_by);
  }
  plan = std::make_unique<ProjectionPlan>(projected, std::move(plan), query.projection_slots);
  if (query.distinct) {
    Schema distinct_schema = plan->OutputSchema();
    plan = std::make_unique<DistinctPlan>(std::move(distinct_schema), std::move(plan));
  }
  if (query.limit.has_value() || query.offset.has_value()) {
    Schema limit_schema = plan->OutputSchema();
    plan = std::make_unique<LimitPlan>(std::move(limit_schema), std::move(plan), query.limit,
                                       query.offset);
  }
  return plan;
}

}  // namespace ontodb
