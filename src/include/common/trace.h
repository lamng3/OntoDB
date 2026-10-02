#pragma once

#include <atomic>
#include <iostream>
#include <mutex>
#include <string_view>

namespace ontodb {

// Sink for `\trace`. Components call Event; they do not print.
// The shell installs a sink that prints only while tracing is on.
//
// Call sites (component name, then a short message):
//   "disk"      DiskManager read/write of a page id
//   "io"        DiskScheduler dequeue
//   "bpm"       fetch hit/miss, new page, unpin, flush
//   "replacer"  LRUK eviction choice and k-distance
//   "btree"     split, merge, redistribute
//   "lock"      grant, wait, deadlock abort
//   "log"       append and flush up to an LSN
class TraceSink {
 public:
  virtual ~TraceSink() = default;
  virtual void Event(std::string_view component, std::string_view message) = 0;
};

class NullTraceSink : public TraceSink {
 public:
  void Event(std::string_view, std::string_view) override {}
};

class PrintingTraceSink : public TraceSink {
 public:
  void Event(std::string_view component, std::string_view message) override {
    std::lock_guard<std::mutex> guard(mu_);
    std::cerr << component << ": " << message << '\n';
  }

 private:
  std::mutex mu_;
};

// Stable object the shell can hand to components. SetEnabled flips output
// without replacing the pointer those components already hold.
class SwitchableTraceSink : public TraceSink {
 public:
  void SetEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }
  auto enabled() const -> bool { return enabled_.load(std::memory_order_relaxed); }

  void Event(std::string_view component, std::string_view message) override {
    if (enabled()) {
      printer_.Event(component, message);
    }
  }

 private:
  std::atomic<bool> enabled_{false};
  PrintingTraceSink printer_;
};

}  // namespace ontodb
