#include "store/mem_triple_iterator.h"

namespace ontodb {

MemTripleIterator::MemTripleIterator(std::vector<Triple> matches) : matches_(std::move(matches)) {}

void MemTripleIterator::Init() {
  index_ = 0;
}

auto MemTripleIterator::Next(Triple* out) -> bool {
  if (index_ >= matches_.size()) {
    return false;
  }
  *out = matches_[index_++];
  return true;
}

}  // namespace ontodb
