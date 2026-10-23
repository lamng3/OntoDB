#pragma once

#include <memory>
#include <optional>

#include "execution/executor_context.h"
#include "execution/executors/abstract_executor.h"
#include "execution/plans/triple_scan_plan.h"
#include "store/triple_iterator.h"

namespace ontodb {

// Under a transaction (PLAN 7.4), a scan locks what it reads: IS on the
// database, then IS on the triple set (S at repeatable read, which keeps any
// insert out of what was scanned), then S on each triple before it returns
// it. Read uncommitted takes no shared locks. Shared locks are held to the
// end of the transaction at every level that takes them: the isolation specs
// require read committed to keep them too (lock-upgrade, upgrade-deadlock).
// If a triple's lock had to wait, the scan checks the triple is still there:
// the writer it waited for may have aborted and taken it back.
class TripleScanExecutor : public AbstractExecutor {
 public:
  TripleScanExecutor(ExecutorContext* ctx, const TripleScanPlan* plan);

  void Init() override;
  // Opens the scan with each variable that `bindings` already binds filled in
  // as a constant, so the store sees the narrowest pattern it can.
  void InitWith(const Row& bindings) override;
  auto Next(Row* row) -> bool override;
  auto GetOutputSchema() const -> const Schema& override;

 private:
  auto BindPosition(const BoundTerm& term, term_id_t value, Row* row) const -> bool;
  auto Vacuous() const -> bool;
  auto LockRow(const Triple& triple) -> bool;  // false if the triple vanished while waiting

  ExecutorContext* ctx_;
  const TripleScanPlan* plan_;
  std::unique_ptr<TripleIterator> iter_;
  bool yielded_vacuous_{false};
  uint64_t scanned_{0};
};

}  // namespace ontodb
