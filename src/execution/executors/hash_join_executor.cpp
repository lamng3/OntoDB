#include "execution/executors/hash_join_executor.h"

#include <algorithm>
#include <iterator>
#include <set>

#include "execution/row_merge.h"

namespace ontodb {

HashJoinExecutor::HashJoinExecutor(std::unique_ptr<AbstractExecutor> left,
                                   std::unique_ptr<AbstractExecutor> right,
                                   const HashJoinPlan* plan)
    : left_(std::move(left)), right_(std::move(right)), plan_(plan) {
  const auto left_slots = ProducedSlots(plan_->GetChildAt(0));
  const auto right_slots = ProducedSlots(plan_->GetChildAt(1));
  std::set_intersection(left_slots.begin(), left_slots.end(), right_slots.begin(),
                        right_slots.end(), std::back_inserter(key_slots_));
  width_ = plan_->OutputSchema().columns.size();
}

auto HashJoinExecutor::KeyHash::operator()(const std::vector<term_id_t>& key) const noexcept
    -> size_t {
  size_t h = 0;
  for (const term_id_t id : key) {
    h ^= std::hash<term_id_t>{}(id) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  }
  return h;
}

auto HashJoinExecutor::KeyOf(const Row& row, std::vector<term_id_t>* key) const -> bool {
  key->clear();
  for (const size_t slot : key_slots_) {
    if (slot >= row.size() || row[slot] == INVALID_TERM_ID) {
      return false;
    }
    key->push_back(row[slot]);
  }
  return true;
}

// Build: read the whole inner input once.
void HashJoinExecutor::Init() {
  table_.clear();
  unkeyed_.clear();
  right_->Init();
  Row row;
  std::vector<term_id_t> key;
  while (right_->Next(&row)) {
    if (KeyOf(row, &key)) {
      table_[key].push_back(row);
    } else {
      unkeyed_.push_back(row);
    }
  }
  left_->Init();
  left_ok_ = left_->Next(&left_row_);
  StartProbe();
}

void HashJoinExecutor::StartProbe() {
  candidates_.clear();
  next_candidate_ = 0;
  if (!left_ok_) {
    return;
  }
  std::vector<term_id_t> key;
  if (KeyOf(left_row_, &key)) {
    if (const auto it = table_.find(key); it != table_.end()) {
      for (const auto& row : it->second) {
        candidates_.push_back(&row);
      }
    }
  } else {
    for (const auto& [unused, rows] : table_) {
      for (const auto& row : rows) {
        candidates_.push_back(&row);
      }
    }
  }
  for (const auto& row : unkeyed_) {
    candidates_.push_back(&row);
  }
}

auto HashJoinExecutor::Next(Row* row) -> bool {
  while (left_ok_) {
    while (next_candidate_ < candidates_.size()) {
      if (MergeRows(left_row_, *candidates_[next_candidate_++], width_, row)) {
        return true;
      }
    }
    left_ok_ = left_->Next(&left_row_);
    StartProbe();
  }
  return false;
}

auto HashJoinExecutor::GetOutputSchema() const -> const Schema& {
  return plan_->OutputSchema();
}

}  // namespace ontodb
