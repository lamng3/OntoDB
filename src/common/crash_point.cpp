#include "common/crash_point.h"

#include <mutex>
#include <string>
#include <utility>

#include "common/exception.h"

namespace ontodb {
namespace {

std::mutex g_mu;
std::string g_armed;
std::function<void(std::string_view)> g_probe;

}  // namespace

void CrashPoint::Reach(std::string_view name) {
  std::function<void(std::string_view)> probe;
  bool armed = false;
  {
    std::lock_guard<std::mutex> guard(g_mu);
    probe = g_probe;
    armed = !g_armed.empty() && g_armed == name;
  }
  if (probe) {
    probe(name);
  }
  if (armed) {
    throw CrashInjected(std::string(name));
  }
}

void CrashPoint::Arm(std::string_view name) {
  std::lock_guard<std::mutex> guard(g_mu);
  g_armed = std::string(name);
}

void CrashPoint::Disarm() {
  std::lock_guard<std::mutex> guard(g_mu);
  g_armed.clear();
  g_probe = nullptr;
}

void CrashPoint::SetProbe(std::function<void(std::string_view)> probe) {
  std::lock_guard<std::mutex> guard(g_mu);
  g_probe = std::move(probe);
}

}  // namespace ontodb
