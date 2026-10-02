#pragma once

#include <string>
#include <vector>

#include "binder/bound_query.h"
#include "execution/executor_context.h"

namespace ontodb {

struct StatementResult {
  bool is_query{false};
  std::vector<std::string> columns;
  std::vector<std::vector<std::string>> rows;
  std::string message;
  std::string plan;
  double milliseconds{0};
};

// Runs a bound statement. SELECT goes through the volcano executors.
// INSERT DATA and DELETE DATA write the memory store directly until
// InsertExecutor and DeleteExecutor exist (PLAN 4.6).
class ExecutionEngine {
 public:
  explicit ExecutionEngine(ExecutorContext* ctx);

  auto Execute(const BoundQuery& query) -> StatementResult;
  auto Explain(const BoundQuery& query) const -> std::string;
  auto ExplainAnalyze(const BoundQuery& query) -> std::string;

 private:
  auto Decode(term_id_t id) const -> std::string;
  static auto CountMessage(const char* verb, size_t n) -> std::string;

  ExecutorContext* ctx_;
};

}  // namespace ontodb
