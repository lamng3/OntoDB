#pragma once

#include <vector>

#include "store/triple_iterator.h"

namespace ontodb {

// Snapshot of the triples that matched when the scan was opened.
class MemTripleIterator : public TripleIterator {
 public:
  explicit MemTripleIterator(std::vector<Triple> matches);

  void Init() override;
  auto Next(Triple* out) -> bool override;

 private:
  std::vector<Triple> matches_;
  size_t index_{0};
};

}  // namespace ontodb
