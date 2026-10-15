#include "execution/row_merge.h"

#include "execution/plans/triple_scan_plan.h"

namespace ontodb {

auto MergeRows(const Row& left, const Row& right, size_t width, Row* out) -> bool {
  out->assign(width, INVALID_TERM_ID);
  for (size_t i = 0; i < width; i++) {
    const bool left_set = i < left.size() && left[i] != INVALID_TERM_ID;
    const bool right_set = i < right.size() && right[i] != INVALID_TERM_ID;
    if (left_set && right_set && left[i] != right[i]) {
      return false;
    }
    (*out)[i] = left_set ? left[i] : (right_set ? right[i] : INVALID_TERM_ID);
  }
  return true;
}

auto RowHash::operator()(const std::vector<term_id_t>& ids) const noexcept -> size_t {
  size_t h = ids.size();
  for (const term_id_t id : ids) {
    h ^= std::hash<term_id_t>{}(id) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  }
  return h;
}

auto ProducedSlots(const AbstractPlanNode* plan) -> std::set<size_t> {
  std::set<size_t> slots;
  if (plan->GetType() == PlanType::kTripleScan) {
    const auto& pattern = static_cast<const TripleScanPlan*>(plan)->Pattern();
    for (const BoundTerm* term : {&pattern.subject, &pattern.predicate, &pattern.object}) {
      if (term->is_variable) {
        slots.insert(term->slot);
      }
    }
    return slots;
  }
  for (const auto& child : plan->GetChildren()) {
    const auto below = ProducedSlots(child.get());
    slots.insert(below.begin(), below.end());
  }
  return slots;
}

}  // namespace ontodb
