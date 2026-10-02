#pragma once

#include <optional>
#include <string>
#include <vector>

#include "buffer/buffer_pool_manager.h"
#include "storage/index/index_iterator.h"
#include "storage/index/triple_key.h"

namespace ontodb {

// B+ tree index of TripleKeys. Duplicate insert returns false. Remove of a
// missing key returns false. GetValue is a point lookup.
//
// Begin() walks every leaf via sibling links. Begin(key, prefix_len) starts
// at the first key covered by that prefix and stops at PrefixUpper. An empty
// tree, an empty prefix match, and a prefix that crosses a leaf boundary are
// all valid. The iterator holds one leaf guard.
//
// Insert and Remove keep every internal separator consistent with the leaves.
// Call these crash points when the operation has written more than one page:
//   BPlusTree::Insert::after_leaf_split
//   BPlusTree::Insert::after_internal_split
//   BPlusTree::Remove::after_redistribute
//   BPlusTree::Remove::after_merge
// Reach them only after those pages and their log records agree.
//
// CheckInvariants returns nullopt when the tree is well formed, otherwise a
// description of the first broken invariant. ToString is a human dump.
// ToDot is a Graphviz digraph. BulkLoad builds a packed tree from keys that
// are already sorted and unique. The tree is single-threaded until PLAN 7.1.
class BPlusTree {
 public:
  BPlusTree(std::string name, page_id_t header_page_id, BufferPoolManager* bpm,
            TraceSink* trace = nullptr);

  auto IsEmpty() const -> bool;
  auto Insert(const TripleKey& key) -> bool;
  auto Remove(const TripleKey& key) -> bool;
  auto GetValue(const TripleKey& key) const -> bool;
  auto Begin() -> IndexIterator;
  auto Begin(const TripleKey& key, int prefix_len) -> IndexIterator;
  auto End() -> IndexIterator;
  void BulkLoad(const std::vector<TripleKey>& keys);
  auto ToString() const -> std::string;
  auto ToDot() const -> std::string;
  auto CheckInvariants() const -> std::optional<std::string>;

 private:
  std::string name_;
  page_id_t header_page_id_;
  BufferPoolManager* bpm_;
  TraceSink* trace_;
};

}  // namespace ontodb
