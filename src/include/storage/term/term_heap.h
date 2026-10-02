#pragma once

#include <string>
#include <string_view>

#include "buffer/buffer_pool_manager.h"
#include "common/rid.h"
#include "storage/term/term_heap_iterator.h"

namespace ontodb {

// A heap of variable-length term spellings. Insert returns the RID of the new
// record. Get returns the bytes at that RID. Delete returns false if the RID
// is not live. Records may span the heap's page chain but not a single record
// larger than a page; Insert of a record that cannot fit on an empty page
// throws StorageException. Begin/End visit every live record once.
class TermHeap {
 public:
  TermHeap(BufferPoolManager* bpm, page_id_t first_page);
  auto Insert(std::string_view bytes) -> RID;
  auto Get(const RID& rid) const -> std::string;
  auto Delete(const RID& rid) -> bool;
  auto Begin() const -> TermHeapIterator;
  auto End() const -> TermHeapIterator;

 private:
  friend class TermHeapIterator;
  BufferPoolManager* bpm_;
  page_id_t first_page_;
};

}  // namespace ontodb
