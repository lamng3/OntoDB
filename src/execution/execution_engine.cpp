#include "execution/execution_engine.h"

#include <chrono>
#include <utility>

#include "common/exception.h"
#include "dictionary/dictionary.h"
#include "execution/executor_factory.h"
#include "planner/planner.h"
#include "store/triple_store.h"

namespace ontodb {

ExecutionEngine::ExecutionEngine(ExecutorContext* ctx) : ctx_(ctx) {}

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

auto ExecutionEngine::Explain(const BoundQuery& query) const -> std::string {
  return Planner().Plan(query)->ToString();
}

auto ExecutionEngine::ExplainAnalyze(const BoundQuery&) -> std::string {
  throw NotImplementedException("PLAN 5.5: ExecutionEngine::ExplainAnalyze");
}

auto ExecutionEngine::Execute(const BoundQuery& query) -> StatementResult {
  const auto started = std::chrono::steady_clock::now();
  StatementResult result;
  const auto plan = Planner().Plan(query);
  result.plan = plan->ToString();

  if (query.kind == BoundQuery::Kind::kInsert || query.kind == BoundQuery::Kind::kDelete) {
    size_t changed = 0;
    for (const auto& triple : query.ground_triples) {
      if (query.kind == BoundQuery::Kind::kInsert) {
        changed += ctx_->GetStore()->Insert(triple) ? 1 : 0;
      } else {
        changed += ctx_->GetStore()->Delete(triple) ? 1 : 0;
      }
    }
    result.message =
        CountMessage(query.kind == BoundQuery::Kind::kInsert ? "inserted" : "deleted", changed);
  } else {
    if (ctx_->GetConfig()->join == JoinMethod::kIndexNestedLoop) {
      throw NotImplementedException("PLAN 4.2: IndexNestedLoopJoinExecutor");
    }
    if (ctx_->GetConfig()->join == JoinMethod::kHash) {
      throw NotImplementedException("PLAN 4.3: HashJoinExecutor");
    }
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
