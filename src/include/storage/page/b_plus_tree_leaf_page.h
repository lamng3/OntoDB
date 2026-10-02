#pragma once

#include "storage/index/triple_key.h"
#include "storage/page/b_plus_tree_page.h"

namespace ontodb {

// Leaf page. The key is the triple; there is no separate payload. Keys are
// strictly increasing. GetNextPageId is the right sibling, or INVALID_PAGE_ID
// on the rightmost leaf. MaxSize() is at least 2.
class BPlusTreeLeafPage : public BPlusTreePage {
 public:
  explicit BPlusTreeLeafPage(Page* page);
  static auto MaxSize() -> int;
  void Init(int max_size);
  auto KeyAt(int index) const -> TripleKey;
  void SetKeyAt(int index, const TripleKey& key);
  auto GetNextPageId() const -> page_id_t;
  void SetNextPageId(page_id_t next);
};

}  // namespace ontodb
