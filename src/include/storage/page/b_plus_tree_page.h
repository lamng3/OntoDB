#pragma once

#include "common/types.h"
#include "storage/page/page.h"

namespace ontodb {

// Header shared by internal and leaf index pages. The bytes live in `page`
// after the pageLSN. Document your byte layout in this header for PLAN 2.1.
// GetLSN and SetLSN are the page's pageLSN, not a second copy.
class BPlusTreePage {
 public:
  enum class IndexPageType { kInvalid = 0, kLeaf, kInternal };

  explicit BPlusTreePage(Page* page);

  void Init(IndexPageType type, int max_size);
  auto GetPageType() const -> IndexPageType;
  void SetPageType(IndexPageType type);
  auto IsLeaf() const -> bool;
  auto GetSize() const -> int;
  void SetSize(int size);
  auto GetMaxSize() const -> int;
  void SetMaxSize(int max_size);
  auto GetLSN() const -> lsn_t;
  void SetLSN(lsn_t lsn);
  auto GetPage() const -> Page* { return page_; }

 private:
  Page* page_;
};

}  // namespace ontodb
