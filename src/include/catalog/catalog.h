#pragma once

#include <string_view>

#include "buffer/buffer_pool_manager.h"

namespace ontodb {

// Metadata page. Records the root page id of each index ("spo", "pos", "osp")
// and the dictionary's root. A missing name returns INVALID_PAGE_ID.
// SetIndexRoot replaces the previous root. Flush makes the metadata durable;
// a later Catalog on the same buffer pool sees the values.
//
// An index "root" is the page id of that tree's header page, which in turn
// holds the tree's current root; the catalog changes only when an index is
// created. Layout after the pageLSN:
//
//   offset  size  field
//        8     4  magic "OCAT"
//       12     4  dictionary root page id
//       16     4  number of named entries
//   20+12i     8  entry i name, NUL-padded (at most 8 bytes)
//   28+12i     4  entry i page id
//
// The constructor initializes a zero page and writes it out at once, so the
// file holds the page and no later allocation can hand out its id. A page
// with other contents throws StorageException rather than being overwritten.
// Names longer than 8 bytes throw std::invalid_argument.
class Catalog {
 public:
  explicit Catalog(BufferPoolManager* bpm, page_id_t catalog_page_id = 0);
  auto GetIndexRoot(std::string_view name) const -> page_id_t;
  void SetIndexRoot(std::string_view name, page_id_t root);
  auto GetDictionaryRoot() const -> page_id_t;
  void SetDictionaryRoot(page_id_t page);
  void Flush();

 private:
  BufferPoolManager* bpm_;
  page_id_t catalog_page_id_;
};

}  // namespace ontodb
