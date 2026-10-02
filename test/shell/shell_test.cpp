#include "shell/shell.h"

#include <gtest/gtest.h>

#include <chrono>
#include <sstream>
#include <thread>

#include "common/database.h"
#include "test_util.h"

namespace ontodb {
namespace {

auto RunScript(const std::string& script) -> std::string {
  Database db;
  std::istringstream in(script);
  std::ostringstream out;
  Shell shell(db, in, out);
  shell.Run();
  return out.str();
}

TEST(ShellSmoke, LoadTinyAndQuery) {
  const auto script = "\\load " + test::DataFile("tiny.ttl").string() +
                      "\nSELECT ?name WHERE { ?s a <http://example.edu/univ#Student> . "
                      "?s <http://example.edu/univ#name> ?name }\n\\quit\n";
  const auto out = RunScript(script);
  EXPECT_NE(out.find("loaded 53 triples"), std::string::npos);
  EXPECT_NE(out.find("\"Alice\""), std::string::npos);
  EXPECT_NE(out.find("\"Bob\""), std::string::npos);
  EXPECT_NE(out.find("\"Cara\""), std::string::npos);
}

TEST(Shell, ExplainStatsSetAndUnbuiltCommands) {
  const auto out = RunScript(
      "\\stats\n\\set pool_size 8\n\\set join nlj\n\\set isolation rr\n\\set\n"
      "\\timing on\n\\bpm\n\\tree spo dot\n\\page 1\n\\trace on\n\\begin\n\\commit\n\\abort\n"
      "\\txns\n\\locks\n\\log 4\n\\checkpoint\n\\crash\n\\set backend indexed\n"
      "\\explain SELECT ?s WHERE { ?s <http://ex/p> <http://ex/o> }\n"
      "\\explain analyze SELECT ?s WHERE { ?s <http://ex/p> <http://ex/o> }\n\\help\n\\quit\n");
  EXPECT_NE(out.find("page reads: 0"), std::string::npos);
  EXPECT_NE(out.find("pool_size 8"), std::string::npos);
  EXPECT_NE(out.find("not built yet (PLAN 1.6)"), std::string::npos);
  EXPECT_NE(out.find("not built yet (PLAN 2.2)"), std::string::npos);
  EXPECT_NE(out.find("not built yet (PLAN 7.2)"), std::string::npos);
  EXPECT_NE(out.find("not built yet (PLAN 7.3)"), std::string::npos);
  EXPECT_NE(out.find("not built yet (PLAN 8.2)"), std::string::npos);
  EXPECT_NE(out.find("not built yet (PLAN 8.5)"), std::string::npos);
  EXPECT_NE(out.find("not built yet (PLAN 8.3)"), std::string::npos);
  EXPECT_NE(out.find("not built yet (PLAN 3.4)"), std::string::npos);
  EXPECT_NE(out.find("not built yet (PLAN 5.5)"), std::string::npos);
  EXPECT_NE(out.find("TripleScan"), std::string::npos);
  EXPECT_NE(out.find("\\session"), std::string::npos);
}

TEST(Shell, SessionWaitReasonDoesNotBlockTheCaller) {
  Database db;
  SessionPool pool;
  std::ostringstream wait_log;
  const auto result = pool.Run(
      2,
      [](SessionState& state) {
        state.SetWaiting("X lock held by txn 5");
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        state.ClearWaiting();
        return std::string("done");
      },
      &wait_log);
  EXPECT_EQ(result, "done");
  EXPECT_NE(wait_log.str().find("session 2 waiting: X lock held by txn 5"), std::string::npos);
  pool.SetActive(2);
  EXPECT_EQ(pool.Active(), 2);
}

}  // namespace
}  // namespace ontodb
