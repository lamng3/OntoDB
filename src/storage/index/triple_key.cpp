#include "storage/index/triple_key.h"

#include "common/exception.h"

namespace ontodb {

auto TripleKey::Encode(term_id_t, term_id_t, term_id_t) -> TripleKey {
  throw NotImplementedException("PLAN 0.2: TripleKey::Encode");
}

void TripleKey::Decode(term_id_t*, term_id_t*, term_id_t*) const {
  throw NotImplementedException("PLAN 0.2: TripleKey::Decode");
}

auto TripleKey::Compare(const TripleKey&) const -> int {
  throw NotImplementedException("PLAN 0.2: TripleKey::Compare");
}

auto TripleKey::PrefixLower(const TripleKey&, int) -> TripleKey {
  throw NotImplementedException("PLAN 0.2: TripleKey::PrefixLower");
}

auto TripleKey::PrefixUpper(const TripleKey&, int) -> TripleKey {
  throw NotImplementedException("PLAN 0.2: TripleKey::PrefixUpper");
}

}  // namespace ontodb
