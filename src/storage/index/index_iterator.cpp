#include "storage/index/index_iterator.h"

#include <utility>

#include "buffer/buffer_pool_manager.h"
#include "storage/page/b_plus_tree_leaf_page.h"

namespace ontodb {

IndexIterator::IndexIterator(BufferPoolManager* bpm, page_id_t leaf, int index)
    : IndexIterator(bpm, bpm->FetchPageRead(leaf), index, std::nullopt) {}

IndexIterator::IndexIterator(BufferPoolManager* bpm, page_id_t leaf, int index,
                             const TripleKey& upper)
    : IndexIterator(bpm, bpm->FetchPageRead(leaf), index, upper) {}

IndexIterator::IndexIterator(BufferPoolManager* bpm, ReadPageGuard leaf, int index,
                             std::optional<TripleKey> upper)
    : bpm_(bpm), end_(false), bounded_(upper.has_value()), upper_(upper.value_or(TripleKey{})) {
  Take(std::move(leaf), index);
  Settle();
}

IndexIterator::IndexIterator(IndexIterator&& other) noexcept
    : bpm_(other.bpm_)
    , keys_(std::move(other.keys_))
    , pos_(other.pos_)
    , leaf_(other.leaf_)
    , first_index_(other.first_index_)
    , next_leaf_(other.next_leaf_)
    , end_(other.end_)
    , bounded_(other.bounded_)
    , upper_(other.upper_) {
  other.Finish();
}

auto IndexIterator::operator=(IndexIterator&& other) noexcept -> IndexIterator& {
  if (this != &other) {
    bpm_ = other.bpm_;
    keys_ = std::move(other.keys_);
    pos_ = other.pos_;
    leaf_ = other.leaf_;
    first_index_ = other.first_index_;
    next_leaf_ = other.next_leaf_;
    end_ = other.end_;
    bounded_ = other.bounded_;
    upper_ = other.upper_;
    other.Finish();
  }
  return *this;
}

IndexIterator::~IndexIterator() = default;

auto IndexIterator::IsEnd() const -> bool {
  return end_;
}

auto IndexIterator::operator*() const -> TripleKey {
  return keys_[pos_];
}

auto IndexIterator::operator++() -> IndexIterator& {
  if (!end_) {
    pos_++;
    Settle();
  }
  return *this;
}

auto IndexIterator::operator==(const IndexIterator& other) const -> bool {
  if (end_ || other.end_) {
    return end_ == other.end_;
  }
  return leaf_ == other.leaf_ && first_index_ + pos_ == other.first_index_ + other.pos_;
}

auto IndexIterator::operator!=(const IndexIterator& other) const -> bool {
  return !(*this == other);
}

void IndexIterator::Take(ReadPageGuard leaf, int index) {
  const BPlusTreeLeafPage page(const_cast<Page*>(leaf.View()));
  keys_.clear();
  pos_ = 0;
  leaf_ = leaf.GetPageId();
  first_index_ = index;
  next_leaf_ = page.GetNextPageId();
  for (int i = index; i < page.GetSize(); i++) {
    const TripleKey key = page.KeyAt(i);
    if (bounded_ && key.Compare(upper_) >= 0) {
      next_leaf_ = INVALID_PAGE_ID;  // past the range: no need to read further
      break;
    }
    keys_.push_back(key);
  }
}  // the latch and the pin are released here

void IndexIterator::Settle() {
  while (!end_ && pos_ >= keys_.size()) {
    if (next_leaf_ == INVALID_PAGE_ID) {
      Finish();
      return;
    }
    Take(bpm_->FetchPageRead(next_leaf_), 0);
  }
}

void IndexIterator::Finish() {
  keys_.clear();
  pos_ = 0;
  leaf_ = INVALID_PAGE_ID;
  first_index_ = 0;
  next_leaf_ = INVALID_PAGE_ID;
  end_ = true;
}

}  // namespace ontodb
