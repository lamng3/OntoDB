#pragma once

#include "storage/page/page.h"

namespace ontodb {

// First page of an index. Holds the current root page id. An empty tree
// stores INVALID_PAGE_ID. The id survives restart.
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
