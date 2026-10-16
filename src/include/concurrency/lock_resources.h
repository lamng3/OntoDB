#pragma once

#include "concurrency/lock_manager.h"

namespace ontodb {

// The resources statements lock. Locks name logical triples, not index
// entries: a triple is one resource however many permutations store it, so
// the lock hierarchy is the database, then the set of triples (named "spo"),
// then one triple (keyed by its SPO key). The same hierarchy serves the memory
// store, which has no indexes at all.
inline auto DatabaseResource() -> LockResource {
  return LockResource{ResourceType::kDatabase, "", TripleKey{}};
}

inline auto TriplesResource() -> LockResource {
  return LockResource{ResourceType::kIndex, "spo", TripleKey{}};
}

inline auto TripleResource(const Triple& t) -> LockResource {
  return LockResource{ResourceType::kKey, "spo",
                      TripleKey::Encode(t.subject, t.predicate, t.object)};
}

}  // namespace ontodb
