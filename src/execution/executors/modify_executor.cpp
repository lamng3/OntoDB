#include "execution/executors/modify_executor.h"

#include "store/triple_store.h"

namespace ontodb {

ModifyExecutor::ModifyExecutor(ExecutorContext* ctx, const AbstractPlanNode* plan, bool insert)
    : ctx_(ctx)
    , plan_(plan)
    , triples_(insert ? &static_cast<const InsertPlan*>(plan)->Triples()
                      : &static_cast<const DeletePlan*>(plan)->Triples())
    , insert_(insert) {}

void ModifyExecutor::Init() {
  done_ = false;
}

auto ModifyExecutor::Next(Row* row) -> bool {
  if (done_) {
    return false;
  }
  done_ = true;
  TripleStore* store = ctx_->GetStore();
  std::vector<Triple> changed;
  try {
    for (const auto& triple : *triples_) {
      if (insert_ ? store->Insert(triple) : store->Delete(triple)) {
        changed.push_back(triple);
      }
    }
  } catch (...) {
    for (auto it = changed.rbegin(); it != changed.rend(); ++it) {
      insert_ ? store->Delete(*it) : store->Insert(*it);
    }
    throw;
  }
  *row = Row{static_cast<term_id_t>(changed.size())};
  return true;
}

auto ModifyExecutor::GetOutputSchema() const -> const Schema& {
  return plan_->OutputSchema();
}

}  // namespace ontodb
