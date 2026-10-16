#pragma once

#include <optional>

#include "storage/index/triple_key.h"
#include "storage/page/page_guard.h"

namespace ontodb {

class BufferPoolManager;

// Forward iterator over a leaf chain. It holds at most one leaf guard at a
// time. Copying is not allowed; moving transfers the guard. Destroying the
// iterator drops that guard, so the pinned-frame count returns to what it
// was before the iterator was created. operator* on an end iterator is
// undefined. ++ past the last key yields the end iterator. With an upper
// bound, the iterator ends at the first key >= upper. Moving to the sibling
// drops the current leaf before pinning the next, so at most one leaf is
// pinned.
class IndexIterator {
 public:
  IndexIterator() = default;
  IndexIterator(BufferPoolManager* bpm, page_id_t leaf, int index);
  IndexIterator(BufferPoolManager* bpm, page_id_t leaf, int index, const TripleKey& upper);
  // Starts on a leaf the caller has already latched (the tree's descent), so
  // the leaf cannot change between finding the position and reading it.
  IndexIterator(BufferPoolManager* bpm, ReadPageGuard leaf, int index,
                std::optional<TripleKey> upper);
  IndexIterator(const IndexIterator&) = delete;
  auto operator=(const IndexIterator&) -> IndexIterator& = delete;
  IndexIterator(IndexIterator&& other) noexcept;
  auto operator=(IndexIterator&& other) noexcept -> IndexIterator&;
  ~IndexIterator();

  auto IsEnd() const -> bool;
  auto operator*() const -> TripleKey;
  auto operator++() -> IndexIterator&;
  auto operator==(const IndexIterator& other) const -> bool;
  auto operator!=(const IndexIterator& other) const -> bool;

 private:
  void Settle();  // skip exhausted leaves, then stop at the upper bound
  void Finish();

  BufferPoolManager* bpm_{nullptr};
  ReadPageGuard guard_;
  page_id_t leaf_{INVALID_PAGE_ID};
  int index_{0};
  bool end_{true};
  bool bounded_{false};
  TripleKey upper_{};
};

}  // namespace ontodb
