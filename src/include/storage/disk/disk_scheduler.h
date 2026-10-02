#pragma once

#include <future>

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
class DiskScheduler {
 public:
  explicit DiskScheduler(DiskManager* disk_manager);
  ~DiskScheduler();

  DiskScheduler(const DiskScheduler&) = delete;
  auto operator=(const DiskScheduler&) -> DiskScheduler& = delete;

  void Schedule(DiskRequest request);

 private:
  DiskManager* disk_manager_;
};

}  // namespace ontodb
