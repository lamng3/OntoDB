#pragma once

#include <utility>
#include <vector>

#include "buffer/buffer_pool_manager.h"
#include "common/types.h"
#include "semantic/class_hierarchy.h"
#include "storage/index/b_plus_tree.h"

namespace ontodb {

// An index from a postorder label to the members that carry it, kept as a
// B+ tree of keys (label, member, 0). The members of a class's subtree are
// then a few range scans, one per interval of the class
// (docs/semantic-search.md, section 3). Used two ways: with the classes
// themselves as members, and with the instances, labelled by the label of an
// rdf:type they have.
class LabelIndex {
 public:
  struct ScanStats {
    size_t scans{0};  // range scans started
    size_t keys{0};   // keys read
  };

  // A new empty tree on its own header page.
  explicit LabelIndex(BufferPoolManager* bpm);
  // An existing tree.
  LabelIndex(BufferPoolManager* bpm, page_id_t header);

  auto Header() const -> page_id_t { return header_; }
  auto Insert(uint32_t label, term_id_t member) -> bool;
  auto Remove(uint32_t label, term_id_t member) -> bool;
  // Into an empty index: sort, drop duplicates, and bulk load.
  void BulkLoad(std::vector<std::pair<uint32_t, term_id_t>> pairs);
  auto Tree() -> BPlusTree* { return &tree_; }

  // The distinct members whose label lies in any of `intervals`, ascending.
  auto Scan(const std::vector<Interval>& intervals, ScanStats* stats = nullptr) -> std::vector<term_id_t>;

 private:
  static auto Key(uint32_t label, term_id_t member) -> TripleKey;

  page_id_t header_;
  BPlusTree tree_;
};

}  // namespace ontodb
