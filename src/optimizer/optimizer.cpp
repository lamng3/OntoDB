#include "optimizer/optimizer.h"

#include <algorithm>
#include <map>
#include <utility>

#include "common/exception.h"
#include "execution/plans/nested_loop_join_plan.h"
#include "execution/plans/stub_plans.h"
#include "execution/plans/triple_scan_plan.h"

namespace ontodb {

CardinalityEstimator::CardinalityEstimator(const Statistics* stats) : stats_(stats) {}

auto CardinalityEstimator::EstimateTriplePattern(const BoundTriplePattern& pattern) const
    -> double {
  if (pattern.entailed) {
    return pattern.entailed_rows;
  }
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

auto Optimizer::RewriteTypeScans(PlanNodeRef plan, const TypeEntailment& entailment) -> PlanNodeRef {
  auto children = plan->ReleaseChildren();
  for (auto& child : children) {
    child = RewriteTypeScans(std::move(child), entailment);
  }
  plan->SetChildren(std::move(children));
  if (plan->GetType() != PlanType::kTripleScan) {
    return plan;
  }
  auto* scan = static_cast<TripleScanPlan*>(plan.get());
  BoundTriplePattern& pattern = scan->MutablePattern();
  const term_id_t type = entailment.RdfType();
  if (type == INVALID_TERM_ID || pattern.predicate.is_variable || pattern.predicate.id != type ||
      pattern.object.is_variable || pattern.object.id == INVALID_TERM_ID ||
      !entailment.Covers(pattern.object.id)) {
    return plan;
  }
  pattern.entailed = true;
  pattern.entailed_rows = entailment.EstimateInstances(pattern.object.id);
  return plan;
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

namespace {

void Flatten(PlanNodeRef node, std::vector<PlanNodeRef>* leaves) {
  if (node->GetType() != PlanType::kNestedLoopJoin) {
    leaves->push_back(std::move(node));
    return;
  }
  auto children = node->ReleaseChildren();
  Flatten(std::move(children[0]), leaves);
  Flatten(std::move(children[1]), leaves);
}

auto VariableSlots(const BoundTriplePattern& p) -> std::vector<size_t> {
  std::vector<size_t> slots;
  for (const BoundTerm* term : {&p.subject, &p.predicate, &p.object}) {
    if (term->is_variable && std::find(slots.begin(), slots.end(), term->slot) == slots.end()) {
      slots.push_back(term->slot);
    }
  }
  return slots;
}

}  // namespace

auto Optimizer::Optimize(PlanNodeRef plan) -> PlanNodeRef {
  return ApplyJoinMethod(Reorder(std::move(plan)), config_->join);
}

auto Optimizer::Reorder(PlanNodeRef plan) -> PlanNodeRef {
  if (plan->GetType() != PlanType::kNestedLoopJoin) {
    auto children = plan->ReleaseChildren();
    for (auto& child : children) {
      child = Reorder(std::move(child));
    }
    plan->SetChildren(std::move(children));
    return plan;
  }
  const Schema schema = plan->OutputSchema();
  const size_t width = static_cast<const NestedLoopJoinPlan*>(plan.get())->RowWidth();
  std::vector<PlanNodeRef> leaves;
  Flatten(std::move(plan), &leaves);
  std::vector<const BoundTriplePattern*> patterns;
  for (const auto& leaf : leaves) {
    if (leaf->GetType() != PlanType::kTripleScan) {
      patterns.clear();  // not a chain of scans: keep the order it came in
      break;
    }
    patterns.push_back(&static_cast<const TripleScanPlan*>(leaf.get())->Pattern());
  }

  // One rebuilt join: whether the inner pattern shares a variable with the
  // outer side, and the estimated rows on each side.
  struct JoinStep {
    bool connected;
    double outer_rows;
    double inner_rows;
  };
  std::vector<JoinStep> steps;
  std::vector<size_t> order;
  if (patterns.size() == leaves.size()) {
    std::vector<bool> used(leaves.size(), false);
    double rows = 0;
    std::map<size_t, double> distinct;  // variable slot -> distinct values so far
    for (size_t step = 0; step < leaves.size(); step++) {
      size_t best = leaves.size();
      double best_rows = 0;
      bool best_connected = false;
      for (size_t i = 0; i < leaves.size(); i++) {
        if (used[i]) {
          continue;
        }
        const double inner = estimator_->EstimateTriplePattern(*patterns[i]);
        double joined = inner;
        bool connected = false;
        if (step > 0) {
          joined = rows * inner;
          for (const size_t slot : VariableSlots(*patterns[i])) {
            if (const auto it = distinct.find(slot); it != distinct.end()) {
              connected = true;
              joined /=
                  std::max({it->second, estimator_->EstimateDistinct(*patterns[i], slot), 1.0});
            }
          }
        }
        const bool better = best == leaves.size() || (connected && !best_connected) ||
                            (connected == best_connected && joined < best_rows);
        if (better) {
          best = i;
          best_rows = joined;
          best_connected = connected;
        }
      }
      if (step > 0) {
        steps.push_back({best_connected, rows, estimator_->EstimateTriplePattern(*patterns[best])});
      }
      used[best] = true;
      order.push_back(best);
      rows = best_rows;
      for (const size_t slot : VariableSlots(*patterns[best])) {
        const double v = estimator_->EstimateDistinct(*patterns[best], slot);
        const auto it = distinct.find(slot);
        distinct[slot] = it == distinct.end() ? v : std::min(it->second, v);
      }
      for (auto& [slot, v] : distinct) {
        v = std::min(v, rows);
      }
    }
  } else {
    for (size_t i = 0; i < leaves.size(); i++) {
      order.push_back(i);
    }
  }

  PlanNodeRef chain = std::move(leaves[order[0]]);
  for (size_t i = 1; i < order.size(); i++) {
    PlanNodeRef inner = std::move(leaves[order[i]]);
    if (config_->join == JoinMethod::kAuto && !steps.empty() && steps[i - 1].connected) {
      if (steps[i - 1].outer_rows <= steps[i - 1].inner_rows) {
        chain =
            std::make_unique<IndexNestedLoopJoinPlan>(schema, std::move(chain), std::move(inner));
      } else {
        chain = std::make_unique<HashJoinPlan>(schema, std::move(chain), std::move(inner));
      }
      continue;
    }
    chain = std::make_unique<NestedLoopJoinPlan>(schema, std::move(chain), std::move(inner), width);
  }
  return chain;
}

}  // namespace ontodb
