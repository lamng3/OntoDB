#include "common/crash_point.h"

#include <gtest/gtest.h>

#include "common/exception.h"

namespace ontodb {
namespace {

TEST(CrashPoint, NoopUntilArmed) {
  CrashPoint::Disarm();
  CrashPoint::Reach("nothing");
  CrashPoint::Arm("BPlusTree::Insert::after_leaf_split");
  EXPECT_THROW(CrashPoint::Reach("BPlusTree::Insert::after_leaf_split"), CrashInjected);
  CrashPoint::Disarm();
  CrashPoint::Reach("BPlusTree::Insert::after_leaf_split");
}

TEST(CrashPoint, ProbeSeesTheName) {
  CrashPoint::Disarm();
  std::string seen;
  CrashPoint::SetProbe([&](std::string_view name) { seen = std::string(name); });
  CrashPoint::Reach("BufferPoolManager::FlushPage::before_disk_write");
  EXPECT_EQ(seen, "BufferPoolManager::FlushPage::before_disk_write");
  CrashPoint::Disarm();
}

}  // namespace
}  // namespace ontodb
