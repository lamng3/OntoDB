#include "stats/statistics.h"

#include "common/exception.h"

namespace ontodb {

void Statistics::Collect(const TripleStore&, const Dictionary&) {
  throw NotImplementedException("PLAN 5.1: Statistics::Collect");
}
auto Statistics::TripleCount() const -> uint64_t {
  throw NotImplementedException("PLAN 5.1: Statistics::TripleCount");
}
auto Statistics::DistinctCount(std::string_view) const -> uint64_t {
  throw NotImplementedException("PLAN 5.1: Statistics::DistinctCount");
}
auto Statistics::PredicateCount(term_id_t) const -> uint64_t {
  throw NotImplementedException("PLAN 5.1: Statistics::PredicateCount");
}
auto Statistics::PatternCount(const TriplePattern&) const -> uint64_t {
  throw NotImplementedException("PLAN 5.1: Statistics::PatternCount");
}

}  // namespace ontodb
