#pragma once

#include <optional>
#include <vector>

#include "storage/index/triple_key.h"
#include "storage/page/page_guard.h"

namespace ontodb {

class BufferPoolManager;

// Forward iterator over a leaf chain. It holds no latch and no pin between
// calls: on reaching a leaf it copies that leaf's remaining keys (stopping at
// the upper bound), notes the right sibling's id, and releases the leaf; it
// latches the sibling only when the copied keys run out. A scan that holds
// nothing between rows cannot be part of a latch deadlock, however its caller
// interleaves it with other scans, writes, or lock waits. Keys present when
// their leaf was read are returned; a key inserted concurrently may or may
// not be. Copying is not allowed; moving transfers the position. operator* on
// an end iterator is undefined. ++ past the last key yields the end iterator.
// With an upper bound, the iterator ends at the first key >= upper.
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
  void Take(ReadPageGuard leaf, int index);  // copy from `index` on, then release
  void Settle();                             // read further leaves until a key or the end
  void Finish();

  BufferPoolManager* bpm_{nullptr};
  std::vector<TripleKey> keys_;  // the current leaf's keys still to return
  size_t pos_{0};
  page_id_t leaf_{INVALID_PAGE_ID};  // the leaf keys_ came from
  int first_index_{0};               // keys_[0]'s slot in that leaf
  page_id_t next_leaf_{INVALID_PAGE_ID};
  bool end_{true};
  bool bounded_{false};
  TripleKey upper_{};
};

}  // namespace ontodb
