#pragma once

#include <string>

namespace ontodb::test {

struct CrashReport {
  bool ok{false};
  std::string message;
};

auto CrashChildMain(int argc, char** argv) -> int;
auto ExecutablePath() -> const std::string&;
void SetExecutablePath(std::string path);

// In-process crash of a correct or deliberately broken store.
auto RunLoseCommit() -> CrashReport;
auto RunKeepUncommitted() -> CrashReport;
auto RunCorrectInProcess() -> CrashReport;
auto RunForked(uint32_t seed) -> CrashReport;

}  // namespace ontodb::test
