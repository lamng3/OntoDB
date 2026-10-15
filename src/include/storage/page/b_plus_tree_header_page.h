#pragma once

#include "storage/page/page.h"

namespace ontodb {

// First page of an index. Holds the current root page id at payload offset 8
// (just after the pageLSN). An empty tree stores INVALID_PAGE_ID. The id
// survives restart. A zero-filled page would read as "root is page 0", which
// is a legal page, so a header is always created with Init, which writes
// INVALID_PAGE_ID.
class BPlusTreeHeaderPage {
 public:
  explicit BPlusTreeHeaderPage(Page* page);
  void Init();
  auto GetRootPageId() const -> page_id_t;
  void SetRootPageId(page_id_t root);

 private:
  Page* page_;
};

}  // namespace ontodb
