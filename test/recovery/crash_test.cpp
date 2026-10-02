#include <gtest/gtest.h>

#include <cstdlib>

#include "recovery/crash_harness.h"

namespace ontodb::test {
namespace {

TEST(CrashHarness, DetectsLostCommittedWrite) {
  const auto report = RunLoseCommit();
  EXPECT_FALSE(report.ok);
  EXPECT_NE(report.message.find("missing"), std::string::npos) << report.message;
}

TEST(CrashHarness, DetectsSurvivingUncommittedWrite) {
  const auto report = RunKeepUncommitted();
  EXPECT_FALSE(report.ok);
  EXPECT_NE(report.message.find("unexpected"), std::string::npos) << report.message;
}

TEST(CrashHarness, AcceptsCorrectStore) {
  const auto report = RunCorrectInProcess();
  EXPECT_TRUE(report.ok) << report.message;
}

TEST(CrashHarness, ForkKillSurvivesOnlyAcknowledgedCommits) {
  const char* env = std::getenv("ONTODB_CRASH_SEEDS");
  const int seeds = env == nullptr ? 2 : std::atoi(env);
  ASSERT_FALSE(ExecutablePath().empty());
  for (int seed = 1; seed <= seeds; seed++) {
    const auto report = RunForked(static_cast<uint32_t>(seed));
    EXPECT_TRUE(report.ok) << "seed " << seed << "\n" << report.message;
  }
}

}  // namespace
}  // namespace ontodb::test
