#pragma once

#include "common/types.h"
#include "storage/page/page.h"

namespace ontodb {

// Header shared by internal and leaf index pages. The bytes live in `page`
// after the pageLSN. GetLSN and SetLSN are the page's pageLSN, not a second
// copy. Integers are stored in the machine's byte order, like the pageLSN.
//
//   offset  size  field
//        0     8  pageLSN (Page)
//        8     4  page type (0 invalid, 1 leaf, 2 internal)
//       12     4  size: keys in a leaf, keys (children - 1) in an internal page
//       16     4  max size
//       20        type-specific layout (see the leaf and internal headers)
class BPlusTreePage {
 public:
  static constexpr size_t kHeaderSize = 20;  // pageLSN plus the three fields above

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
