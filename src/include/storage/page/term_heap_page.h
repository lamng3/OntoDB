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
class TermHeapPage {
 public:
  explicit TermHeapPage(Page* page);
  void Init();
  auto Insert(std::string_view bytes) -> std::optional<slot_offset_t>;
  auto Get(slot_offset_t slot) const -> std::string_view;
  void Delete(slot_offset_t slot);
  auto GetSlotCount() const -> uint16_t;
  auto FreeSpace() const -> uint16_t;

 private:
  Page* page_;
};

}  // namespace ontodb
