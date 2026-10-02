#pragma once

#include <memory>

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
};

}  // namespace ontodb
