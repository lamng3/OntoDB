#pragma once

#include "storage/index/triple_key.h"
#include "storage/page/page_guard.h"

namespace ontodb {

class BufferPoolManager;

// Forward iterator over a leaf chain. It holds at most one leaf guard at a
// time. Copying is not allowed; moving transfers the guard. Destroying the
// iterator drops that guard, so the pinned-frame count returns to what it
// was before the iterator was created. operator* on an end iterator is
// undefined. ++ past the last key yields the end iterator.
class IndexIterator {
 public:
  IndexIterator() = default;
  IndexIterator(BufferPoolManager* bpm, page_id_t leaf, int index);
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
  BufferPoolManager* bpm_{nullptr};
  page_id_t leaf_{INVALID_PAGE_ID};
  int index_{0};
  bool end_{true};
};

}  // namespace ontodb
