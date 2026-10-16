#pragma once

#include <memory>
#include <vector>

#include "common/types.h"
#include "store/triple_iterator.h"

namespace ontodb {

// RDF graph as a set of triples. Insert of a triple that is already present
// returns false and leaves the graph unchanged. Delete of an absent triple
// returns false. Scan filters only the bound positions of `pattern`; variable
// constraints inside one pattern are the scan executor's job.
class TripleStore {
 public:
  virtual ~TripleStore() = default;

  virtual auto Insert(const Triple& triple) -> bool = 0;
  virtual auto Delete(const Triple& triple) -> bool = 0;
  virtual auto Scan(const TriplePattern& pattern) -> std::unique_ptr<TripleIterator> = 0;

  // Inserts many triples and returns how many were new. The default inserts
  // them one at a time; a store may do better (the indexed store bulk-loads
  // its trees when they are empty).
  virtual auto InsertBatch(const std::vector<Triple>& triples) -> size_t {
    size_t added = 0;
    for (const auto& triple : triples) {
      added += Insert(triple) ? 1 : 0;
    }
    return added;
  }
};

}  // namespace ontodb
