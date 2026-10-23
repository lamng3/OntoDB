#include "execution/execution_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <sstream>
#include <unordered_map>
#include <utility>

#include "common/exception.h"
#include "dictionary/dictionary.h"
#include "execution/executor_factory.h"
#include "execution/plans/stub_plans.h"
#include "execution/plans/triple_scan_plan.h"
#include "optimizer/optimizer.h"
#include "planner/planner.h"
#include "store/triple_store.h"

namespace ontodb {

ExecutionEngine::ExecutionEngine(ExecutorContext* ctx, const Statistics* statistics)
    : ctx_(ctx), statistics_(statistics) {}

auto ExecutionEngine::CountMessage(const char* verb, size_t n) -> std::string {
  std::string text = verb;
  text += " ";
  text += std::to_string(n);
  text += n == 1 ? " triple" : " triples";
  return text;
}

auto ExecutionEngine::Decode(term_id_t id) const -> std::string {
  if (id == INVALID_TERM_ID) {
    return "";
  }
  if (const auto text = ctx_->GetDictionary()->Lookup(id)) {
    return *text;
  }
  return "?";
}

// The plan that runs: the planner's tree, optimized when statistics exist,
// otherwise with only the join operator \set join forces. \explain prints
// exactly this tree.
auto ExecutionEngine::PlanFor(const BoundQuery& query) const -> PlanNodeRef {
  auto plan = Planner().Plan(query);
  if (ctx_->GetConfig()->entail && ctx_->GetEntailment() != nullptr) {
    plan = Optimizer::RewriteTypeScans(std::move(plan), *ctx_->GetEntailment());
  }
  if (statistics_ == nullptr) {
    return Optimizer::ApplyJoinMethod(std::move(plan), ctx_->GetConfig()->join);
  }
  const CardinalityEstimator estimator(statistics_);
  return Optimizer(&estimator, ctx_->GetConfig()).Optimize(std::move(plan));
}

auto ExecutionEngine::Explain(const BoundQuery& query) const -> std::string {
  return PlanFor(query)->ToString();
}

namespace {

// A node's estimated rows and, per variable slot, its estimated distinct values.
struct NodeEstimate {
  double rows{0};
  std::map<size_t, double> distinct;
};

// The same estimates the optimizer plans with: exact counts for scans, System R
// with distinct values for joins. A filter keeps a third of its input (the
// classic System R default for a comparison), a limit caps its child, and the
// rest pass their child's estimate through.
auto Estimate(const AbstractPlanNode* node, const CardinalityEstimator& estimator,
              std::unordered_map<const AbstractPlanNode*, double>* out) -> NodeEstimate {
  NodeEstimate result;
  switch (node->GetType()) {
    case PlanType::kTripleScan: {
      const auto& pattern = static_cast<const TripleScanPlan*>(node)->Pattern();
      result.rows = estimator.EstimateTriplePattern(pattern);
      for (const BoundTerm* term : {&pattern.subject, &pattern.predicate, &pattern.object}) {
        if (term->is_variable) {
          result.distinct[term->slot] = estimator.EstimateDistinct(pattern, term->slot);
        }
      }
      break;
    }
    case PlanType::kNestedLoopJoin:
    case PlanType::kIndexNestedLoopJoin:
    case PlanType::kHashJoin: {
      const auto left = Estimate(node->GetChildAt(0), estimator, out);
      const auto right = Estimate(node->GetChildAt(1), estimator, out);
      result.rows = left.rows * right.rows;
      result.distinct = left.distinct;
      for (const auto& [slot, v] : right.distinct) {
        if (const auto it = left.distinct.find(slot); it != left.distinct.end()) {
          result.rows /= std::max({it->second, v, 1.0});
          result.distinct[slot] = std::min(it->second, v);
        } else {
          result.distinct[slot] = v;
        }
      }
      for (auto& [slot, v] : result.distinct) {
        v = std::min(v, result.rows);
      }
      break;
    }
    case PlanType::kLimit: {
      result = Estimate(node->GetChildAt(0), estimator, out);
      const auto* limit = static_cast<const LimitPlan*>(node);
      result.rows = std::max(0.0, result.rows - static_cast<double>(limit->Offset().value_or(0)));
      if (const auto cap = limit->Limit()) {
        result.rows = std::min(result.rows, static_cast<double>(*cap));
      }
      break;
    }
    case PlanType::kInsert:
      result.rows = static_cast<double>(static_cast<const InsertPlan*>(node)->Triples().size());
      break;
    case PlanType::kDelete:
      result.rows = static_cast<double>(static_cast<const DeletePlan*>(node)->Triples().size());
      break;
    default:
      result = Estimate(node->GetChildAt(0), estimator, out);
      if (node->GetType() == PlanType::kFilter) {
        result.rows /= 3;
      }
      break;
  }
  (*out)[node] = result.rows;
  return result;
}

// The inner scan of an index nested-loop join never runs as an executor: the
// join probes the store with it once per outer row, so it has no count.
void PrintAnalyzed(const AbstractPlanNode* node, int depth, bool probed,
                   const std::unordered_map<const AbstractPlanNode*, double>& estimates,
                   const RowCounts& counts, std::ostringstream* out) {
  *out << std::string(static_cast<size_t>(depth) * 2, ' ') << node->Label() << "  (estimated ";
  if (const auto it = estimates.find(node); it != estimates.end()) {
    *out << static_cast<uint64_t>(std::llround(it->second));
  } else {
    *out << '?';
  }
  if (probed) {
    *out << ", probed by the join above)\n";
  } else {
    const auto actual = counts.find(node);
    *out << ", actual " << (actual == counts.end() ? 0 : actual->second) << ")\n";
  }
  const auto& children = node->GetChildren();
  for (size_t i = 0; i < children.size(); i++) {
    const bool inner_probe = node->GetType() == PlanType::kIndexNestedLoopJoin && i == 1;
    PrintAnalyzed(children[i].get(), depth + 1, inner_probe, estimates, counts, out);
  }
}

}  // namespace

// Runs the plan with every executor counting its rows, then prints each node
// with its estimate and the rows it actually returned. Updates are not run.
auto ExecutionEngine::ExplainAnalyze(const BoundQuery& query) -> std::string {
  const auto plan = PlanFor(query);
  std::unordered_map<const AbstractPlanNode*, double> estimates;
  if (statistics_ != nullptr) {
    const CardinalityEstimator estimator(statistics_);
    Estimate(plan.get(), estimator, &estimates);
  }
  RowCounts counts;
  const auto started = std::chrono::steady_clock::now();
  if (query.kind != BoundQuery::Kind::kInsert && query.kind != BoundQuery::Kind::kDelete) {
    auto executor = ExecutorFactory::Create(plan.get(), ctx_, &counts);
    executor->Init();
    Row row;
    while (executor->Next(&row)) {
    }
  }
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  std::ostringstream out;
  PrintAnalyzed(plan.get(), 0, false, estimates, counts, &out);
  out << "execution: " << ms << " ms\n";
  return out.str();
}

auto ExecutionEngine::Execute(const BoundQuery& query) -> StatementResult {
  const auto started = std::chrono::steady_clock::now();
  StatementResult result;
  const auto plan = PlanFor(query);
  result.plan = plan->ToString();

  if (query.kind == BoundQuery::Kind::kInsert || query.kind == BoundQuery::Kind::kDelete) {
    auto executor = ExecutorFactory::Create(plan.get(), ctx_);
    executor->Init();
    Row count;
    const size_t changed = executor->Next(&count) ? static_cast<size_t>(count.at(0)) : 0;
    result.message =
        CountMessage(query.kind == BoundQuery::Kind::kInsert ? "inserted" : "deleted", changed);
  } else {
    auto executor = ExecutorFactory::Create(plan.get(), ctx_);
    executor->Init();
    result.is_query = true;
    for (const auto& column : executor->GetOutputSchema().columns) {
      result.columns.push_back(column.name);
    }
    Row row;
    while (executor->Next(&row)) {
      std::vector<std::string> decoded;
      decoded.reserve(row.size());
      for (const term_id_t id : row) {
        decoded.push_back(Decode(id));
      }
      result.rows.push_back(std::move(decoded));
    }
  }

  const auto finished = std::chrono::steady_clock::now();
  result.milliseconds = std::chrono::duration<double, std::milli>(finished - started).count();
  return result;
}

}  // namespace ontodb
