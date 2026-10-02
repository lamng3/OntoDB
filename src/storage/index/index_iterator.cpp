#include "storage/index/index_iterator.h"

#include "common/exception.h"

namespace ontodb {

IndexIterator::IndexIterator(BufferPoolManager*, page_id_t, int) {
  throw NotImplementedException("PLAN 2.5: IndexIterator::IndexIterator");
}

IndexIterator::IndexIterator(IndexIterator&& other) noexcept
    : bpm_(other.bpm_), leaf_(other.leaf_), index_(other.index_), end_(other.end_) {
  other.end_ = true;
  other.bpm_ = nullptr;
}

auto IndexIterator::operator=(IndexIterator&& other) noexcept -> IndexIterator& {
  if (this != &other) {
    bpm_ = other.bpm_;
    leaf_ = other.leaf_;
    index_ = other.index_;
    end_ = other.end_;
    other.end_ = true;
    other.bpm_ = nullptr;
  }
  return *this;
}

IndexIterator::~IndexIterator() = default;

auto IndexIterator::IsEnd() const -> bool {
  return end_;
}

auto IndexIterator::operator*() const -> TripleKey {
  throw NotImplementedException("PLAN 2.5: IndexIterator::operator*");
}

auto IndexIterator::operator++() -> IndexIterator& {
  throw NotImplementedException("PLAN 2.5: IndexIterator::operator++");
}

auto IndexIterator::operator==(const IndexIterator& other) const -> bool {
  return end_ == other.end_ && leaf_ == other.leaf_ && index_ == other.index_;
}

auto IndexIterator::operator!=(const IndexIterator& other) const -> bool {
  return !(*this == other);
}

}  // namespace ontodb
