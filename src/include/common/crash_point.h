#pragma once

#include <functional>
#include <string_view>

namespace ontodb {

// Test hook for crash-recovery tests. Reach is a no-op unless a test armed
// the name or installed a probe. Multi-page operations call Reach at the
// points named in their headers, after the log is consistent with the pages
// written so far and before the next page write.
class CrashPoint {
 public:
  static void Reach(std::string_view name);
  static void Arm(std::string_view name);
  static void Disarm();
  static void SetProbe(std::function<void(std::string_view)> probe);
};

}  // namespace ontodb
