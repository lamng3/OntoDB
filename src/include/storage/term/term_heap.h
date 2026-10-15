#pragma once

#include <optional>
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
//
// The heap is a chain of TermHeapPages starting at first_page; a zeroed first
// page is initialized on construction. Insert appends to the last page of the
// chain and adds a page when the record does not fit; space freed on earlier
// pages is reused only by records that land there. Get of a RID that is not
// live throws StorageException. Not thread-safe; the dictionary serializes.
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
  // The first live record strictly after `from`, or the first one at all when
  // `from` is the invalid RID.
  auto NextLive(const RID& from) const -> std::optional<RID>;
  auto LastPage() -> page_id_t;

  BufferPoolManager* bpm_;
  page_id_t first_page_;
  page_id_t last_page_{INVALID_PAGE_ID};  // found on the first Insert
};

}  // namespace ontodb
