#include "storage/disk/disk_scheduler.h"

#include "common/exception.h"

namespace ontodb {

DiskScheduler::DiskScheduler(DiskManager* disk_manager) : disk_manager_(disk_manager) {
  throw NotImplementedException("PLAN 1.2: DiskScheduler::DiskScheduler");
}

DiskScheduler::~DiskScheduler() = default;

void DiskScheduler::Schedule(DiskRequest) {
  throw NotImplementedException("PLAN 1.2: DiskScheduler::Schedule");
}

}  // namespace ontodb
