#pragma once

#include <string>
#include <utility>

#include "common/rid.h"

namespace ontodb {

class TermHeap;

// Walks every live record. operator* returns the RID and a copy of the bytes.
// The iterator does not hold a pin after operator* returns.
class TermHeapIterator {
 public:
  TermHeapIterator() = default;
  auto operator*() const -> std::pair<RID, std::string>;
  auto operator++() -> TermHeapIterator&;
  auto operator==(const TermHeapIterator& other) const -> bool;
  auto IsEnd() const -> bool;

 private:
  friend class TermHeap;
  const TermHeap* heap_{nullptr};
  RID rid_{};
  bool end_{true};
};

}  // namespace ontodb
