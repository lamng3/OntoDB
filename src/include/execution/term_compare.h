#pragma once

#include "binder/bound_query.h"
#include "common/types.h"

namespace ontodb {

class Dictionary;

// = and != use numeric equality when both sides are numeric literals, and
// term identity otherwise. Ordered comparisons are numeric for numeric
// literals and lexicographic for two terms of the same kind. A comparison
// that SPARQL would treat as a type error is false.
auto CompareTerms(term_id_t left_id, term_id_t right_id, BoundFilter::Op op,
                  const Dictionary& dictionary) -> bool;

}  // namespace ontodb
