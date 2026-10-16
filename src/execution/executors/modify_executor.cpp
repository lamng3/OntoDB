#include "execution/executors/modify_executor.h"

#include "concurrency/lock_resources.h"
#include "concurrency/transaction.h"
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
  Transaction* txn = ctx_->GetTransaction();
  LockManager* locks = ctx_->GetLockManager();
  std::vector<Triple> changed;
  try {
    if (txn != nullptr) {
      locks->Lock(txn, DatabaseResource(), LockMode::kIx, ctx_->GetSession());
      locks->Lock(txn, TriplesResource(), LockMode::kIx, ctx_->GetSession());
    }
    for (const auto& triple : *triples_) {
      if (txn != nullptr) {
        locks->Lock(txn, TripleResource(triple), LockMode::kX, ctx_->GetSession());
      }
      if (insert_ ? store->Insert(triple) : store->Delete(triple)) {
        changed.push_back(triple);
        if (txn != nullptr) {
          txn->GetWriteSet().push_back({insert_, triple});
        }
      }
    }
  } catch (...) {
    for (auto it = changed.rbegin(); it != changed.rend(); ++it) {
      insert_ ? store->Delete(*it) : store->Insert(*it);
      if (txn != nullptr) {
        txn->GetWriteSet().pop_back();
      }
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
