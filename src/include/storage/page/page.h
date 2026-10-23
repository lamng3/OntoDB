#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include "common/config.h"
#include "common/types.h"

namespace ontodb {

// A reader-writer latch for one page. std::shared_mutex keeps its reader count behind an internal
// mutex, so many readers of one hot page (the root of a tree) queue on that mutex; this latch is
// one atomic word. Bits 0-29 count readers, bit 30 says a writer is waiting (new readers stay
// out, so a writer is not starved), bit 31 says a writer holds it. It spins briefly and then
// yields; a latch is held for the length of one page operation, never across I/O.
class RwLatch {
 public:
  void lock_shared() {
    for (int spins = 0;; spins++) {
      uint32_t s = state_.load(std::memory_order_relaxed);
      if ((s & (kWriter | kWaiting)) == 0 &&
          state_.compare_exchange_weak(s, s + 1, std::memory_order_acquire, std::memory_order_relaxed)) {
        return;
      }
      Backoff(spins);
    }
  }
  void unlock_shared() { state_.fetch_sub(1, std::memory_order_release); }
  void lock() {
    for (int spins = 0;; spins++) {
      uint32_t s = state_.load(std::memory_order_relaxed);
      if ((s & kWriter) == 0) {
        if ((s & kReaders) == 0) {
          if (state_.compare_exchange_weak(s, (s & ~kWaiting) | kWriter, std::memory_order_acquire,
                                           std::memory_order_relaxed)) {
            return;
          }
          continue;
        }
        if ((s & kWaiting) == 0) {
          state_.fetch_or(kWaiting, std::memory_order_relaxed);
        }
      }
      Backoff(spins);
    }
  }
  void unlock() { state_.fetch_and(~kWriter, std::memory_order_release); }
  void Reset() { state_.store(0, std::memory_order_relaxed); }

 private:
  static constexpr uint32_t kReaders = (1U << 30) - 1;
  static constexpr uint32_t kWaiting = 1U << 30;
  static constexpr uint32_t kWriter = 1U << 31;
  static void Backoff(int spins) {
    if (spins > 32) {
      std::this_thread::yield();
    }
  }
  std::atomic<uint32_t> state_{0};
};

// One buffer-pool frame. Bytes [0, 8) of data_ are the on-disk pageLSN.
// Pin count, dirty bit, and page id live beside the image; they are not
// written by DiskManager. DiskManager copies GetData() for PAGE_SIZE bytes.
// The latch is a reader-writer lock on the frame's bytes, taken by page guards
// after the pin and released before the unpin. It is never waited on while
// the pool's own latch is held. A Page is neither copyable nor movable.
//
// The pin count, page id, dirty bit, and referenced bit are atomic so that a pool with several
// shards can pin a resident page without taking its shard's latch (see BufferPoolManager). A pin
// count of -1 means the pool has claimed the frame to evict or delete it: no one may pin it.
class Page {
 public:
  Page() = default;
  Page(const Page&) = delete;
  auto operator=(const Page&) -> Page& = delete;
  Page(Page&&) = delete;
  auto operator=(Page&&) -> Page& = delete;

  auto GetData() -> char* { return data_; }
  auto GetData() const -> const char* { return data_; }

  auto GetPageId() const -> page_id_t { return page_id_.load(std::memory_order_acquire); }
  void SetPageId(page_id_t page_id) { page_id_.store(page_id, std::memory_order_release); }

  auto GetPinCount() const -> int { return pin_count_.load(std::memory_order_acquire); }
  void IncrementPin() { pin_count_.fetch_add(1, std::memory_order_acq_rel); }
  void DecrementPin() { pin_count_.fetch_sub(1, std::memory_order_acq_rel); }
  // Adds a pin unless the frame is claimed (-1); false means the caller must take the slow path.
  auto TryPin() -> bool {
    int c = pin_count_.load(std::memory_order_relaxed);
    while (c >= 0) {
      if (pin_count_.compare_exchange_weak(c, c + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        return true;
      }
    }
    return false;
  }
  // Removes a pin if there is one.
  auto TryUnpin() -> bool {
    int c = pin_count_.load(std::memory_order_relaxed);
    while (c > 0) {
      if (pin_count_.compare_exchange_weak(c, c - 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        return true;
      }
    }
    return false;
  }
  // Claims an unpinned frame (0 to -1) so that it can be evicted or deleted.
  auto TryClaim() -> bool {
    int expected = 0;
    return pin_count_.compare_exchange_strong(expected, -1, std::memory_order_acq_rel);
  }
  void Release() { pin_count_.store(0, std::memory_order_release); }

  auto IsDirty() const -> bool { return is_dirty_.load(std::memory_order_relaxed); }
  void SetDirty(bool dirty) { is_dirty_.store(dirty, std::memory_order_relaxed); }

  // Set by a hit that took no latch; the pool folds it into the replacer when it must evict.
  auto Referenced() const -> bool { return referenced_.load(std::memory_order_relaxed); }
  void MarkReferenced() {
    if (!referenced_.load(std::memory_order_relaxed)) {
      referenced_.store(true, std::memory_order_relaxed);
    }
  }
  auto TakeReferenced() -> bool { return referenced_.exchange(false, std::memory_order_relaxed); }

  void RLatch() { latch_.lock_shared(); }
  void RUnlatch() { latch_.unlock_shared(); }
  void WLatch() { latch_.lock(); }
  void WUnlatch() { latch_.unlock(); }

  auto GetLSN() const -> lsn_t;
  void SetLSN(lsn_t lsn);

  // Called only on an unpinned frame that is about to hold another page (or none). Guards pin
  // before they latch, so no thread holds or waits on the latch.
  void Reset() {
    page_id_.store(INVALID_PAGE_ID, std::memory_order_release);
    pin_count_.store(0, std::memory_order_release);
    is_dirty_.store(false, std::memory_order_relaxed);
    referenced_.store(false, std::memory_order_relaxed);
    for (size_t i = 0; i < PAGE_SIZE; i++) {
      data_[i] = 0;
    }
    latch_.Reset();
  }

 private:
  char data_[PAGE_SIZE]{};
  std::atomic<page_id_t> page_id_{INVALID_PAGE_ID};
  std::atomic<int> pin_count_{0};
  std::atomic<bool> is_dirty_{false};
  std::atomic<bool> referenced_{false};
  RwLatch latch_;
};

}  // namespace ontodb
