#pragma once

#include "common/types.h"

namespace ontodb {

// One pass over a triple scan. Init restarts the pass. Next writes the next
// matching triple and returns false when the scan is exhausted.
// Destroying an iterator releases every resource it holds. After it is gone
// the buffer pool's pinned-frame count is unchanged by this scan.
class TripleIterator {
 public:
  virtual ~TripleIterator() = default;
  virtual void Init() = 0;
  virtual auto Next(Triple* out) -> bool = 0;
};

}  // namespace ontodb
