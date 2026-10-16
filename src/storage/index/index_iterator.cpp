#include "storage/index/index_iterator.h"

#include "buffer/buffer_pool_manager.h"
#include "storage/page/b_plus_tree_leaf_page.h"

namespace ontodb {

namespace {

auto Leaf(const ReadPageGuard& guard) -> BPlusTreeLeafPage {
  // The wrapper only reads through this pointer here.
  return BPlusTreeLeafPage(const_cast<Page*>(guard.View()));
}

}  // namespace

IndexIterator::IndexIterator(BufferPoolManager* bpm, page_id_t leaf, int index)
    : bpm_(bpm), leaf_(leaf), index_(index), end_(false) {
  guard_ = bpm_->FetchPageRead(leaf_);
  Settle();
}

IndexIterator::IndexIterator(BufferPoolManager* bpm, page_id_t leaf, int index,
                             const TripleKey& upper)
    : bpm_(bpm), leaf_(leaf), index_(index), end_(false), bounded_(true), upper_(upper) {
  guard_ = bpm_->FetchPageRead(leaf_);
  Settle();
}

IndexIterator::IndexIterator(BufferPoolManager* bpm, ReadPageGuard leaf, int index,
                             std::optional<TripleKey> upper)
    : bpm_(bpm)
    , guard_(std::move(leaf))
    , leaf_(guard_.GetPageId())
    , index_(index)
    , end_(false)
    , bounded_(upper.has_value())
    , upper_(upper.value_or(TripleKey{})) {
  Settle();
}

IndexIterator::IndexIterator(IndexIterator&& other) noexcept
    : bpm_(other.bpm_)
    , guard_(std::move(other.guard_))
    , leaf_(other.leaf_)
    , index_(other.index_)
    , end_(other.end_)
    , bounded_(other.bounded_)
    , upper_(other.upper_) {
  other.end_ = true;
  other.bpm_ = nullptr;
  other.leaf_ = INVALID_PAGE_ID;
  other.index_ = 0;
}

auto IndexIterator::operator=(IndexIterator&& other) noexcept -> IndexIterator& {
  if (this != &other) {
    guard_ = std::move(other.guard_);
    bpm_ = other.bpm_;
    leaf_ = other.leaf_;
    index_ = other.index_;
    end_ = other.end_;
    bounded_ = other.bounded_;
    upper_ = other.upper_;
    other.end_ = true;
    other.bpm_ = nullptr;
    other.leaf_ = INVALID_PAGE_ID;
    other.index_ = 0;
  }
  return *this;
}

IndexIterator::~IndexIterator() = default;

auto IndexIterator::IsEnd() const -> bool {
  return end_;
}

auto IndexIterator::operator*() const -> TripleKey {
  return Leaf(guard_).KeyAt(index_);
}

auto IndexIterator::operator++() -> IndexIterator& {
  if (!end_) {
    index_++;
    Settle();
  }
  return *this;
}

auto IndexIterator::operator==(const IndexIterator& other) const -> bool {
  return end_ == other.end_ && leaf_ == other.leaf_ && index_ == other.index_;
}

auto IndexIterator::operator!=(const IndexIterator& other) const -> bool {
  return !(*this == other);
}

void IndexIterator::Settle() {
  while (!end_) {
    const auto leaf = Leaf(guard_);
    if (index_ < leaf.GetSize()) {
      if (bounded_ && leaf.KeyAt(index_).Compare(upper_) >= 0) {
        Finish();
      }
      return;
    }
    const page_id_t next = leaf.GetNextPageId();
    guard_.Drop();  // drop this leaf before pinning the sibling
    if (next == INVALID_PAGE_ID) {
      Finish();
      return;
    }
    guard_ = bpm_->FetchPageRead(next);
    leaf_ = next;
    index_ = 0;
  }
}

void IndexIterator::Finish() {
  guard_.Drop();
  end_ = true;
  leaf_ = INVALID_PAGE_ID;
  index_ = 0;
}

}  // namespace ontodb
