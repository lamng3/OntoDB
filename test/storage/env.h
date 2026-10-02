#pragma once

#include "buffer/buffer_pool_manager.h"
#include "common/crash_point.h"
#include "common/io_stats.h"
#include "storage/disk/disk_manager.h"
#include "test_util.h"

namespace ontodb::test {

struct DiskEnv {
  TempDir dir;
  IoStats stats;
  DiskManager disk;
  BufferPoolManager bpm;

  explicit DiskEnv(size_t pool = 8, size_t k = 2, TraceSink* trace = nullptr)
      : disk((dir.path() / "db").string(), &stats, trace)
      , bpm(pool, &disk, k, nullptr, &stats, trace) {}
};

struct ArmGuard {
  explicit ArmGuard(std::string_view name) { CrashPoint::Arm(name); }
  ~ArmGuard() { CrashPoint::Disarm(); }
  ArmGuard(const ArmGuard&) = delete;
  auto operator=(const ArmGuard&) -> ArmGuard& = delete;
};

}  // namespace ontodb::test
