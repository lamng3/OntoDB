#pragma once

#include <memory>
#include <vector>

#include "buffer/buffer_pool_manager.h"
#include "execution/executor_context.h"
#include "execution/executors/abstract_executor.h"
#include "execution/plans/stub_plans.h"
#include "execution/term_compare.h"

namespace ontodb {

// ORDER BY. Rows compare key by key with TermOrder (values, not ids), and
// equal rows keep their input order: the sort is stable.
//
// Rows that fit in half the buffer pool (at least four pages' worth) are
// sorted in memory. Otherwise the sort is external: it cuts the input into
// sorted runs of that size and writes each run to a chain of pages in the
// pool, then merges runs, at most (budget - 2) at a time, until the last
// merge can stream its rows straight to Next. Run pages are freed as soon as
// they are read, and any left over (when the consumer stops early) when the
// executor is re-initialized or destroyed. Without a buffer pool (the memory
// backend), every sort is in memory.
//
// Run page layout after the pageLSN: next page id (4 bytes), row count
// (4 bytes), then rows of `width` ids, 8 bytes each.
class SortExecutor : public AbstractExecutor {
 public:
  SortExecutor(ExecutorContext* ctx, std::unique_ptr<AbstractExecutor> child, const SortPlan* plan);
  ~SortExecutor() override;
  SortExecutor(const SortExecutor&) = delete;
  auto operator=(const SortExecutor&) -> SortExecutor& = delete;

  void Init() override;
  auto Next(Row* row) -> bool override;
  auto GetOutputSchema() const -> const Schema& override;

  // How the last Init sorted: 0 runs means in memory; otherwise the number of
  // runs written and of merge passes before the final one. For tests.
  auto RunsWritten() const -> size_t { return runs_written_; }
  auto MergePasses() const -> size_t { return merge_passes_; }

 private:
  class RunReader;

  auto Less(const Row& a, const Row& b) -> bool;
  auto WriteRun(const std::vector<Row>& rows) -> page_id_t;
  auto MergeToRun(std::vector<page_id_t> runs) -> page_id_t;
  auto Smallest() -> int;  // index of the reader with the smallest row, or -1
  void Release();

  std::unique_ptr<AbstractExecutor> child_;
  const SortPlan* plan_;
  BufferPoolManager* bpm_;
  TermOrder order_;
  size_t width_{0};
  std::vector<Row> rows_;  // the in-memory sort
  size_t next_{0};
  std::vector<std::unique_ptr<RunReader>> readers_;  // the final merge
  std::vector<page_id_t> owned_;                     // run pages not yet freed
  size_t runs_written_{0};
  size_t merge_passes_{0};
};

}  // namespace ontodb
