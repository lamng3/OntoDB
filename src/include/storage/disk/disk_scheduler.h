#pragma once

#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <optional>
#include <thread>

#include "common/trace.h"
#include "common/types.h"
#include "storage/disk/disk_manager.h"

namespace ontodb {

// One I/O request. The scheduler takes ownership of the promise. `data` must
// stay valid until the promise is set. is_write false means ReadPage.
struct DiskRequest {
  bool is_write{false};
  char* data{nullptr};
  page_id_t page_id{INVALID_PAGE_ID};
  std::promise<bool> callback;
};

// A single background thread drains a queue of DiskRequests and runs them on
// the DiskManager. Schedule returns immediately. The request's promise is set
// once the I/O has finished. Destroying the scheduler finishes queued work
// and joins the thread. Trace component "io".
//
// Requests run in the order they were scheduled, so a write followed by a
// read of the same page reads what was written. If the DiskManager throws,
// the exception is stored in the promise and rethrown by the waiter's get().
class DiskScheduler {
 public:
  explicit DiskScheduler(DiskManager* disk_manager, TraceSink* trace = nullptr);
  ~DiskScheduler();

  DiskScheduler(const DiskScheduler&) = delete;
  auto operator=(const DiskScheduler&) -> DiskScheduler& = delete;

  void Schedule(DiskRequest request);

 private:
  void Run();

  DiskManager* disk_manager_;
  TraceSink* trace_;
  std::mutex mu_;
  std::condition_variable ready_;
  std::deque<std::optional<DiskRequest>> queue_;  // nullopt asks the worker to stop
  std::thread worker_;
};

}  // namespace ontodb
