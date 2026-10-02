#pragma once

#include <functional>

#include "common/types.h"

namespace ontodb {

// Address of a slot on a term-heap page. Value type, not a plan item.
class RID {
 public:
  RID() = default;
  RID(page_id_t page_id, slot_offset_t slot) : page_id_(page_id), slot_(slot) {}

  auto GetPageId() const -> page_id_t { return page_id_; }
  auto GetSlot() const -> slot_offset_t { return slot_; }

  auto operator==(const RID& other) const -> bool {
    return page_id_ == other.page_id_ && slot_ == other.slot_;
  }

 private:
  page_id_t page_id_{INVALID_PAGE_ID};
  slot_offset_t slot_{INVALID_SLOT};
};

}  // namespace ontodb

template<>
struct std::hash<ontodb::RID> {
  auto operator()(const ontodb::RID& rid) const noexcept -> size_t {
    const auto page = static_cast<uint64_t>(static_cast<uint32_t>(rid.GetPageId()));
    return std::hash<uint64_t>{}((page << 16) ^ rid.GetSlot());
  }
};
