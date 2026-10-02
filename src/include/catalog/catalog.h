#pragma once

#include <string_view>

#include "buffer/buffer_pool_manager.h"

namespace ontodb {

// Metadata page. Records the root page id of each index ("spo", "pos", "osp")
// and the dictionary's root. A missing name returns INVALID_PAGE_ID.
// SetIndexRoot replaces the previous root. Flush makes the metadata durable;
// a later Catalog on the same buffer pool sees the values.
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
