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
// are already sorted and unique.
//
// Node sizes default to what a page holds (169 keys per leaf, 144 per
// internal page); tests may pass smaller ones to grow deep trees from few
// keys. Every node but the root keeps at least half its maximum: a leaf at
// least leaf_max / 2 keys, an internal page at least internal_max / 2 keys.
//
// Thread-safe by latch crabbing (PLAN 7.1). Readers latch each child before
// releasing its parent. Insert and Remove latch the header and then the path
// in write mode, releasing every latched ancestor once the newest page is
// safe (cannot split, or cannot underflow), so they hold only what a
// structure change can reach. Latches are taken in one order everywhere: the
// header, then down the tree, and siblings left to right. A split releases its two leaves before
// taking the parent's new page. A leaf emptied by a merge is left in place with its sibling link,
// not freed, because an iterator may hold its id; freed internal pages are unreachable once their
// parent is updated. The diagnostics (ToString, ToDot, CheckInvariants) assume no concurrent
// writer.
class BPlusTree {
 public:
  BPlusTree(std::string name, page_id_t header_page_id, BufferPoolManager* bpm,
            TraceSink* trace = nullptr, int leaf_max_size = 0, int internal_max_size = 0);

  auto IsEmpty() const -> bool;
  auto Insert(const TripleKey& key) -> bool;
  auto Remove(const TripleKey& key) -> bool;
  auto GetValue(const TripleKey& key) const -> bool;
  auto Begin() -> IndexIterator;
  auto Begin(const TripleKey& key, int prefix_len) -> IndexIterator;
  // The keys in [lower, upper): from the first key >= lower to the first key >= upper.
  auto BeginRange(const TripleKey& lower, const TripleKey& upper) -> IndexIterator;
  auto End() -> IndexIterator;
  void BulkLoad(const std::vector<TripleKey>& keys);
  auto ToString() const -> std::string;
  auto ToDot() const -> std::string;
  auto CheckInvariants() const -> std::optional<std::string>;

 private:
  auto GetRoot() const -> page_id_t;
  auto LeafFor(const TripleKey* key) const -> ReadPageGuard;
  auto CheckNode(page_id_t page_id, const std::optional<TripleKey>& lower,
                 const std::optional<TripleKey>& upper, int depth, bool is_root, int* leaf_depth,
                 std::vector<std::pair<page_id_t, page_id_t>>* leaves) const
      -> std::optional<std::string>;
  void Trace(const std::string& message) const;

  std::string name_;
  page_id_t header_page_id_;
  BufferPoolManager* bpm_;
  TraceSink* trace_;
  int leaf_max_;
  int internal_max_;
};

}  // namespace ontodb
