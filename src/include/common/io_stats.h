#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace ontodb {

// Counters behind `\stats`. Thread-safe. This scaffold never increments them.
//
// Call sites, once those components exist:
//   DiskManager::ReadPage            -> PageRead()
//   DiskManager::WritePage           -> PageWrite()
//   BufferPoolManager cache hit      -> BufferHit()
//   BufferPoolManager cache miss     -> BufferMiss()
//   LogManager physical flush        -> LogFlush()
class IoStats {
 public:
  void PageRead() { page_reads_.fetch_add(1, std::memory_order_relaxed); }
  void PageWrite() { page_writes_.fetch_add(1, std::memory_order_relaxed); }
  void BufferHit() { buffer_hits_.fetch_add(1, std::memory_order_relaxed); }
  void BufferMiss() { buffer_misses_.fetch_add(1, std::memory_order_relaxed); }
  void LogFlush() { log_flushes_.fetch_add(1, std::memory_order_relaxed); }

  auto page_reads() const -> uint64_t { return page_reads_.load(std::memory_order_relaxed); }
  auto page_writes() const -> uint64_t { return page_writes_.load(std::memory_order_relaxed); }
  auto buffer_hits() const -> uint64_t { return buffer_hits_.load(std::memory_order_relaxed); }
  auto buffer_misses() const -> uint64_t { return buffer_misses_.load(std::memory_order_relaxed); }
  auto log_flushes() const -> uint64_t { return log_flushes_.load(std::memory_order_relaxed); }

  void Reset() {
    page_reads_.store(0, std::memory_order_relaxed);
    page_writes_.store(0, std::memory_order_relaxed);
    buffer_hits_.store(0, std::memory_order_relaxed);
    buffer_misses_.store(0, std::memory_order_relaxed);
    log_flushes_.store(0, std::memory_order_relaxed);
  }

  auto Format() const -> std::string {
    return "page reads: " + std::to_string(page_reads()) +
           "\npage writes: " + std::to_string(page_writes()) +
           "\nbuffer hits: " + std::to_string(buffer_hits()) +
           "\nbuffer misses: " + std::to_string(buffer_misses()) +
           "\nlog flushes: " + std::to_string(log_flushes());
  }

 private:
  std::atomic<uint64_t> page_reads_{0};
  std::atomic<uint64_t> page_writes_{0};
  std::atomic<uint64_t> buffer_hits_{0};
  std::atomic<uint64_t> buffer_misses_{0};
  std::atomic<uint64_t> log_flushes_{0};
};

}  // namespace ontodb
