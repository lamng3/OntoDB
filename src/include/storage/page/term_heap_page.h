#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "common/types.h"
#include "storage/page/page.h"

namespace ontodb {

// Slotted page of byte strings. Insert returns nullopt when the record does
// not fit. Get's string_view is valid only while the page stays pinned.
// Delete frees the slot; a later Insert may reuse it. Slot ids are stable
// until that slot is deleted. FreeSpace is the bytes still available for a
// new record plus its slot directory entry.
//
//   offset    size  field
//        0       8  pageLSN
//        8       4  next page id in the heap's chain, or INVALID_PAGE_ID
//       12       2  slot count
//       14       2  start of the record area (records grow down from 4096)
//   16 + 4i      4  slot i: record offset (0xFFFF = free) and length
//
// Delete compacts the page, so freed bytes are usable at once and a record
// never moves to another page. The largest record is 4096 - 16 - 4 = 4076
// bytes. A page whose record-area start is 0 was never initialized.
class TermHeapPage {
 public:
  explicit TermHeapPage(Page* page);
  static constexpr size_t kMaxRecordSize = PAGE_SIZE - 16 - 4;

  void Init();
  auto IsInitialized() const -> bool;
  auto IsLive(slot_offset_t slot) const -> bool;
  auto GetNextPageId() const -> page_id_t;
  void SetNextPageId(page_id_t next);
  auto Insert(std::string_view bytes) -> std::optional<slot_offset_t>;
  auto Get(slot_offset_t slot) const -> std::string_view;
  void Delete(slot_offset_t slot);
  auto GetSlotCount() const -> uint16_t;
  auto FreeSpace() const -> uint16_t;

 private:
  Page* page_;
};

}  // namespace ontodb
