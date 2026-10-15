#pragma once

#include <memory>
#include <shared_mutex>

#include "common/config.h"
#include "common/types.h"

namespace ontodb {

// One buffer-pool frame. Bytes [0, 8) of data_ are the on-disk pageLSN.
// Pin count, dirty bit, and page id live beside the image; they are not
// written by DiskManager. DiskManager copies GetData() for PAGE_SIZE bytes.
// The latch is a reader-writer lock on the frame's bytes, taken by page guards
// after the pin and released before the unpin. It is never waited on while
// the pool's own latch is held. A Page is neither copyable nor movable.
class Page {
 public:
  Page() = default;
  Page(const Page&) = delete;
  auto operator=(const Page&) -> Page& = delete;
  Page(Page&&) = delete;
  auto operator=(Page&&) -> Page& = delete;

  auto GetData() -> char* { return data_; }
  auto GetData() const -> const char* { return data_; }

  auto GetPageId() const -> page_id_t { return page_id_; }
  void SetPageId(page_id_t page_id) { page_id_ = page_id; }

  auto GetPinCount() const -> int { return pin_count_; }
  void IncrementPin() { ++pin_count_; }
  void DecrementPin() { --pin_count_; }

  auto IsDirty() const -> bool { return is_dirty_; }
  void SetDirty(bool dirty) { is_dirty_ = dirty; }

  void RLatch() { latch_->lock_shared(); }
  void RUnlatch() { latch_->unlock_shared(); }
  void WLatch() { latch_->lock(); }
  void WUnlatch() { latch_->unlock(); }

  auto GetLSN() const -> lsn_t;
  void SetLSN(lsn_t lsn);

  // Called only on an unpinned frame that is about to hold another page (or
  // none). Guards pin before they latch, so no thread holds or waits on the
  // latch, and the frame gets a fresh one: a latch then belongs to one page,
  // not to every page the frame has held, and lock-order checkers such as
  // ThreadSanitizer do not see an order between unrelated pages. The new latch
  // is a new allocation, made before the old one is freed: ThreadSanitizer
  // forgets a mutex when its memory is freed, but not when a statically
  // initialized std::shared_mutex is destroyed in place.
  void Reset() {
    page_id_ = INVALID_PAGE_ID;
    pin_count_ = 0;
    is_dirty_ = false;
    for (size_t i = 0; i < PAGE_SIZE; i++) {
      data_[i] = 0;
    }
    latch_ = std::make_unique<std::shared_mutex>();
  }

 private:
  char data_[PAGE_SIZE]{};
  page_id_t page_id_{INVALID_PAGE_ID};
  int pin_count_{0};
  bool is_dirty_{false};
  std::unique_ptr<std::shared_mutex> latch_{std::make_unique<std::shared_mutex>()};
};

}  // namespace ontodb
