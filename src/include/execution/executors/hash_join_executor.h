#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "execution/executor_context.h"
#include "execution/executors/abstract_executor.h"
#include "execution/plans/stub_plans.h"

namespace ontodb {

// Builds a hash table on the inner (right) input, keyed by the ids of the
// variables both inputs produce, then probes it with each outer row. A row
// whose key slot is unbound is not hashed: an inner one is kept aside and
// compared with every outer row, an outer one is compared with every inner
// row. Matches are merged as in the nested loop. With no shared variable,
// the key is empty and the join is a cross product.
class HashJoinExecutor : public AbstractExecutor {
 public:
  HashJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right,
                   const HashJoinPlan* plan);

  void Init() override;
  auto Next(Row* row) -> bool override;
  auto GetOutputSchema() const -> const Schema& override;

 private:
  struct KeyHash {
    auto operator()(const std::vector<term_id_t>& key) const noexcept -> size_t;
  };

  auto KeyOf(const Row& row,
             std::vector<term_id_t>* key) const -> bool;  // false if a slot is unbound
  void StartProbe();

  std::unique_ptr<AbstractExecutor> left_;
  std::unique_ptr<AbstractExecutor> right_;
  const HashJoinPlan* plan_;
  std::vector<size_t> key_slots_;
  size_t width_;
  std::unordered_map<std::vector<term_id_t>, std::vector<Row>, KeyHash> table_;
  std::vector<Row> unkeyed_;            // inner rows with an unbound key slot
  std::vector<const Row*> candidates_;  // inner rows to try against left_row_
  size_t next_candidate_{0};
  Row left_row_;
  bool left_ok_{false};
};

}  // namespace ontodb
