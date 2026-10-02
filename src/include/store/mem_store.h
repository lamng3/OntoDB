#pragma once

#include <mutex>
#include <vector>

#include "store/triple_store.h"

namespace ontodb {

// Vector of triples, scanned linearly. The reference iterator and the oracle
// for the indexed store. The mutex lets shell sessions share one store; it
// does not implement a transaction.
class MemStore : public TripleStore {
 public:
  auto Insert(const Triple& triple) -> bool override;
  auto Delete(const Triple& triple) -> bool override;
  auto Scan(const TriplePattern& pattern) -> std::unique_ptr<TripleIterator> override;

  auto Size() const -> size_t;

 private:
  mutable std::mutex mu_;
  std::vector<Triple> triples_;
};

}  // namespace ontodb
