#pragma once

#include "storage/index/triple_key.h"
#include "storage/page/b_plus_tree_page.h"

namespace ontodb {

// Internal page. n keys have n + 1 children. KeyAt(0) is unused. ValueAt(0)
// is the leftmost child. For i > 0, keys in ValueAt(i - 1) are strictly less
// than KeyAt(i), and keys in ValueAt(i) are greater than or equal to KeyAt(i).
// Keys are strictly increasing. MaxSize() is at least 2 and the page fits in
// PAGE_SIZE after the pageLSN.
//
//   offset      size  field
//   20 + 28*i     24  KeyAt(i) (slot 0's key bytes are unused)
//   44 + 28*i      4  ValueAt(i), the child page id
// n keys use n + 1 slots; MaxSize() = (4096 - 20) / 28 - 1 = 144 keys.
class BPlusTreeInternalPage : public BPlusTreePage {
 public:
  explicit BPlusTreeInternalPage(Page* page);
  static auto MaxSize() -> int;
  void Init(int max_size);
  auto KeyAt(int index) const -> TripleKey;
  void SetKeyAt(int index, const TripleKey& key);
  auto ValueAt(int index) const -> page_id_t;
  void SetValueAt(int index, page_id_t page_id);
};

}  // namespace ontodb
