#pragma once

#include <string>
#include <vector>

#include "binder/bound_query.h"
#include "execution/executor_context.h"
#include "execution/plans/abstract_plan.h"
#include "stats/statistics.h"

namespace ontodb {

struct StatementResult {
  bool is_query{false};
  std::vector<std::string> columns;
  std::vector<std::vector<std::string>> rows;
  std::string message;
  std::string plan;
  double milliseconds{0};
};

// Runs a bound statement through the volcano executors: SELECT as a tree of
// scans, joins, and the rest; INSERT DATA and DELETE DATA as one
// ModifyExecutor, whose single output row is the number of triples changed.
class ExecutionEngine {
 public:
  // With statistics, plans go through the optimizer (join order, then the
  // join operator); without, only \set join's forced operator is applied.
  explicit ExecutionEngine(ExecutorContext* ctx, const Statistics* statistics = nullptr);

  auto Execute(const BoundQuery& query) -> StatementResult;
  auto Explain(const BoundQuery& query) const -> std::string;
  auto ExplainAnalyze(const BoundQuery& query) -> std::string;

 private:
  auto PlanFor(const BoundQuery& query) const -> PlanNodeRef;
  auto Decode(term_id_t id) const -> std::string;
  static auto CountMessage(const char* verb, size_t n) -> std::string;

  ExecutorContext* ctx_;
  const Statistics* statistics_;
};

}  // namespace ontodb
