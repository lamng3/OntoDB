#pragma once

#include <set>

#include "common/types.h"
#include "execution/plans/abstract_plan.h"

namespace ontodb {

// Combines two rows of one query into `out` (width `width`). A slot set on
// both sides must hold the same id, or the rows do not join and the result is
// false. INVALID_TERM_ID marks a slot a side has not bound.
auto MergeRows(const Row& left, const Row& right, size_t width, Row* out) -> bool;

// The variable slots a plan's rows bind: a scan's variables, the union of a
// join's children, a filter's or sort's child. Joins use the intersection of
// their children's slots as the join key.
auto ProducedSlots(const AbstractPlanNode* plan) -> std::set<size_t>;

}  // namespace ontodb
