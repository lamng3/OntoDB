#include "execution/executors/update_executor.h"

#include "concurrency/lock_resources.h"
#include "concurrency/transaction.h"
#include "concurrency/transaction_manager.h"
#include "concurrency/version_index.h"
#include "store/triple_store.h"

namespace ontodb {
namespace {

auto Substitute(const BoundTriplePattern& pattern, const Row& row) -> std::optional<Triple> {
  auto id = [&](const BoundTerm& term) -> term_id_t {
    if (!term.is_variable) {
      return term.id;
    }
    return term.slot < row.size() ? row[term.slot] : INVALID_TERM_ID;
  };
  const Triple triple{id(pattern.subject), id(pattern.predicate), id(pattern.object)};
  if (triple.subject == INVALID_TERM_ID || triple.predicate == INVALID_TERM_ID ||
      triple.object == INVALID_TERM_ID) {
    return std::nullopt;
  }
  return triple;
}

auto WriteOne(ExecutorContext* ctx, const Triple& triple, bool insert) -> bool {
  TripleStore* store = ctx->GetStore();
  Transaction* txn = ctx->GetTransaction();
  if (txn != nullptr) {
    ctx->GetLockManager()->Lock(txn, TripleResource(triple), LockMode::kX, ctx->GetSession());
    if (txn->ChecksConflicts() && ctx->GetVersions() != nullptr &&
        ctx->GetVersions()->LastChange(triple) > txn->GetSnapshot().value_or(0)) {
      throw TransactionAbortException("txn " + std::to_string(txn->GetId()) +
                                      ": another transaction committed a change to this triple "
                                      "after this one began");
    }
    TransactionManager* txns = ctx->GetTransactionManager();
    if (txns != nullptr && txns->HasLog() && store->Contains(triple) != insert) {
      txns->LogUpdate(txn, insert, triple);
    }
  }
  if (!(insert ? store->Insert(triple) : store->Delete(triple))) {
    return false;
  }
  if (txn != nullptr) {
    txn->GetWriteSet().push_back({insert, triple});
  }
  return true;
}

}  // namespace

UpdateExecutor::UpdateExecutor(ExecutorContext* ctx, std::unique_ptr<AbstractExecutor> child,
                               const UpdatePlan* plan)
    : ctx_(ctx), child_(std::move(child)), plan_(plan) {}

void UpdateExecutor::Init() {
  done_ = false;
}

auto UpdateExecutor::Next(Row* row) -> bool {
  if (done_) {
    return false;
  }
  done_ = true;
  child_->Init();
  std::vector<Row> solutions;
  Row solution(plan_->Width(), INVALID_TERM_ID);
  while (child_->Next(&solution)) {
    solutions.push_back(solution);
  }
  Transaction* txn = ctx_->GetTransaction();
  if (txn != nullptr) {
    ctx_->GetLockManager()->Lock(txn, DatabaseResource(), LockMode::kIx, ctx_->GetSession());
    ctx_->GetLockManager()->Lock(txn, TriplesResource(), LockMode::kIx, ctx_->GetSession());
  }
  std::vector<Triple> undone;
  size_t changed = 0;
  try {
    for (const auto& solution_row : solutions) {
      for (const auto& pattern : plan_->Deletes()) {
        if (const auto triple = Substitute(pattern, solution_row)) {
          if (WriteOne(ctx_, *triple, false)) {
            undone.push_back(*triple);
            changed++;
          }
        }
      }
    }
    for (const auto& solution_row : solutions) {
      for (const auto& pattern : plan_->Inserts()) {
        if (const auto triple = Substitute(pattern, solution_row)) {
          if (WriteOne(ctx_, *triple, true)) {
            undone.push_back(*triple);
            changed++;
          }
        }
      }
    }
  } catch (...) {
    TripleStore* store = ctx_->GetStore();
    for (auto it = undone.rbegin(); it != undone.rend(); ++it) {
      // The deletes were the first `deletes` changes; putting a deleted triple
      // back is an insert, and taking an inserted one out is a delete. The
      // write set records the original direction, so walk it from the end.
      if (txn != nullptr && !txn->GetWriteSet().empty()) {
        const bool was_insert = txn->GetWriteSet().back().is_insert;
        was_insert ? store->Delete(*it) : store->Insert(*it);
        txn->GetWriteSet().pop_back();
      }
    }
    throw;
  }
  *row = Row{static_cast<term_id_t>(changed)};
  return true;
}

auto UpdateExecutor::GetOutputSchema() const -> const Schema& {
  return plan_->OutputSchema();
}

}  // namespace ontodb
