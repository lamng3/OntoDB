#include <gtest/gtest.h>

#include <string>

namespace ontodb::test {
auto CrashChildMain(int argc, char** argv) -> int;
auto ExecutablePath() -> const std::string&;
void SetExecutablePath(std::string path);
}  // namespace ontodb::test

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--crash-child") {
    return ontodb::test::CrashChildMain(argc, argv);
  }
  ontodb::test::SetExecutablePath(argc > 0 ? argv[0] : "");
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
