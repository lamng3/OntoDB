#pragma once

#include "common/config.h"
#include "common/types.h"

namespace ontodb {

// One buffer-pool frame. Bytes [0, 8) of data_ are the on-disk pageLSN.
// Pin count, dirty bit, and page id live beside the image; they are not
// written by DiskManager. DiskManager copies GetData() for PAGE_SIZE bytes.
class Page {
 public:
  Page() = default;

  auto GetData() -> char* { return data_; }
  auto GetData() const -> const char* { return data_; }

  auto GetPageId() const -> page_id_t { return page_id_; }
  void SetPageId(page_id_t page_id) { page_id_ = page_id; }

  auto GetPinCount() const -> int { return pin_count_; }
  void IncrementPin() { ++pin_count_; }
  void DecrementPin() { --pin_count_; }

  auto IsDirty() const -> bool { return is_dirty_; }
  void SetDirty(bool dirty) { is_dirty_ = dirty; }

  auto GetLSN() const -> lsn_t;
  void SetLSN(lsn_t lsn);

  void Reset() {
    page_id_ = INVALID_PAGE_ID;
    pin_count_ = 0;
    is_dirty_ = false;
    for (size_t i = 0; i < PAGE_SIZE; i++) {
      data_[i] = 0;
    }
  }

 private:
  char data_[PAGE_SIZE]{};
  page_id_t page_id_{INVALID_PAGE_ID};
  int pin_count_{0};
  bool is_dirty_{false};
};

}  // namespace ontodb
