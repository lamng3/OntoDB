#include "optimizer/optimizer.h"

#include <algorithm>
#include <utility>

#include "common/exception.h"
#include "execution/plans/stub_plans.h"

namespace ontodb {

CardinalityEstimator::CardinalityEstimator(const Statistics* stats) : stats_(stats) {}

auto CardinalityEstimator::EstimateTriplePattern(const BoundTriplePattern& pattern) const
    -> double {
  TriplePattern constants;
  for (const auto& [term, out] : {std::pair{&pattern.subject, &constants.subject},
                                  std::pair{&pattern.predicate, &constants.predicate},
                                  std::pair{&pattern.object, &constants.object}}) {
    if (!term->is_variable) {
      if (term->id == INVALID_TERM_ID) {
        return 0;  // a constant the store has never seen matches nothing
      }
      *out = term->id;
    }
  }
  return static_cast<double>(stats_->PatternCount(constants));
}

auto CardinalityEstimator::EstimateDistinct(const BoundTriplePattern& pattern,
                                            size_t slot) const -> double {
  const double rows = EstimateTriplePattern(pattern);
  auto is = [slot](const BoundTerm& term) { return term.is_variable && term.slot == slot; };
  const bool predicate_bound = !pattern.predicate.is_variable;
  double distinct = rows;
  if (is(pattern.subject)) {
    if (predicate_bound && pattern.object.is_variable) {
      distinct = static_cast<double>(stats_->DistinctSubjects(pattern.predicate.id));
    } else if (!predicate_bound) {
      distinct = static_cast<double>(stats_->DistinctCount("s"));
    }
  } else if (is(pattern.object)) {
    if (predicate_bound && pattern.subject.is_variable) {
      distinct = static_cast<double>(stats_->DistinctObjects(pattern.predicate.id));
    } else if (!predicate_bound) {
      distinct = static_cast<double>(stats_->DistinctCount("o"));
    }
  } else if (is(pattern.predicate)) {
    distinct = static_cast<double>(stats_->DistinctCount("p"));
  }
  return std::min(distinct, rows);
}

auto CardinalityEstimator::EstimateJoinWith(double left_rows, double right_rows,
                                            double left_distinct,
                                            double right_distinct) const -> double {
  const double denominator = std::max({left_distinct, right_distinct, 1.0});
  return std::max(0.0, left_rows * right_rows / denominator);
}

auto CardinalityEstimator::EstimateJoin(double left_rows, double right_rows,
                                        size_t shared_vars) const -> double {
  if (shared_vars == 0) {
    return std::max(0.0, left_rows * right_rows);
  }
  double estimate = EstimateJoinWith(left_rows, right_rows, left_rows, right_rows);
  const double subjects = std::max(1.0, static_cast<double>(stats_->DistinctCount("s")));
  for (size_t i = 1; i < shared_vars; i++) {
    estimate /= subjects;
  }
  return estimate;
}

Optimizer::Optimizer(const CardinalityEstimator* estimator, const Config* config)
    : estimator_(estimator), config_(config) {}

auto Optimizer::ApplyJoinMethod(PlanNodeRef plan, JoinMethod method) -> PlanNodeRef {
  if (method != JoinMethod::kHash && method != JoinMethod::kIndexNestedLoop) {
    return plan;
  }
  auto children = plan->ReleaseChildren();
  for (auto& child : children) {
    child = ApplyJoinMethod(std::move(child), method);
  }
  if (plan->GetType() != PlanType::kNestedLoopJoin) {
    plan->SetChildren(std::move(children));
    return plan;
  }
  if (method == JoinMethod::kHash) {
    return std::make_unique<HashJoinPlan>(plan->OutputSchema(), std::move(children[0]),
                                          std::move(children[1]));
  }
  if (children[1]->GetType() == PlanType::kTripleScan) {
    return std::make_unique<IndexNestedLoopJoinPlan>(plan->OutputSchema(), std::move(children[0]),
                                                     std::move(children[1]));
  }
  plan->SetChildren(std::move(children));
  return plan;
}

auto Optimizer::Optimize(PlanNodeRef) -> PlanNodeRef {
  throw NotImplementedException("PLAN 5.3: Optimizer::Optimize");
}

}  // namespace ontodb
