// Snapshot isolation through the Database: what a reader sees, on each backend, against an oracle.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "common/database.h"
#include "common/exception.h"
#include "test_util.h"

namespace ontodb {
namespace {

auto Sorted(const StatementResult& result) -> std::vector<std::string> {
  std::vector<std::string> rows;
  for (const auto& row : result.rows) {
    std::string line;
    for (const auto& value : row) {
      line += value + " ";
    }
    rows.push_back(line);
  }
  std::sort(rows.begin(), rows.end());
  return rows;
}

void SetIsolation(Database& db, IsolationLevel level) {
  auto config = db.GetConfig();
  config.isolation = level;
  db.UpdateConfig(config);
}

auto Spell(int s, int p, int o) -> std::string {
  return "<http://ex/s" + std::to_string(s) + "> <http://ex/p" + std::to_string(p) +
         "> <http://ex/o" + std::to_string(o) + ">";
}

class Mvcc : public ::testing::TestWithParam<BackendKind> {
 protected:
  void SetUp() override {
    if (GetParam() == BackendKind::kIndexed) {
      db_.SetBackend(BackendKind::kIndexed);
    }
    SetIsolation(db_, IsolationLevel::kSnapshot);
  }
  Database db_;
};

TEST_P(Mvcc, ACommitIsOneTimestampAndAReadIsNone) {
  EXPECT_EQ(db_.GetVersions().Clock(), 0U);
  db_.Execute("INSERT DATA { " + Spell(1, 1, 1) + " . " + Spell(2, 1, 1) + " . }", nullptr);
  EXPECT_EQ(db_.GetVersions().Clock(), 1U);
  db_.Execute("SELECT ?s WHERE { ?s ?p ?o }", nullptr);
  db_.Execute("INSERT DATA { " + Spell(1, 1, 1) + " . }", nullptr);  // already there: no change
  EXPECT_EQ(db_.GetVersions().Clock(), 1U);
  db_.Begin();
  db_.Execute("INSERT DATA { " + Spell(3, 1, 1) + " . }", nullptr);
  db_.Execute("INSERT DATA { " + Spell(4, 1, 1) + " . }", nullptr);
  EXPECT_EQ(db_.GetVersions().Clock(), 1U);
  db_.Commit();
  EXPECT_EQ(db_.GetVersions().Clock(), 2U);
  EXPECT_EQ(db_.GetVersions().Count(1), 2U);
  EXPECT_EQ(db_.GetVersions().Count(2), 4U);
}

TEST_P(Mvcc, AnAbortCommitsNothing) {
  db_.Execute("INSERT DATA { " + Spell(1, 1, 1) + " . }", nullptr);
  db_.Begin();
  db_.Execute("DELETE DATA { " + Spell(1, 1, 1) + " . }", nullptr);
  db_.Abort();
  EXPECT_EQ(db_.GetVersions().Clock(), 1U);
  EXPECT_EQ(Sorted(db_.Execute("SELECT ?s WHERE { ?s ?p ?o }", nullptr)).size(), 1U);
}

TEST_P(Mvcc, ALoadIsOneCommit) {
  const auto file = std::filesystem::temp_directory_path() / "ontodb-mvcc-load.nt";
  {
    std::ofstream out(file);
    for (int i = 0; i < 50; i++) {
      out << Spell(i, 1, 1) << " .\n";
    }
    out << Spell(0, 1, 1) << " .\n";  // a repeat is not a change
  }
  db_.Execute("INSERT DATA { " + Spell(0, 1, 1) + " . }", nullptr);
  db_.Load(file.string());
  std::filesystem::remove(file);
  EXPECT_EQ(db_.GetVersions().Clock(), 2U);
  EXPECT_EQ(db_.GetVersions().Count(1), 1U);
  EXPECT_EQ(db_.GetVersions().Count(2), 50U);
  EXPECT_EQ(db_.GetVersions().SpanCount(), 50U);
}

// A random history at snapshot isolation, on one thread: each snapshot must keep answering as the
// state it began on, whichever join method runs, while later commits change the database. The
// oracle is a set for each commit; the answers are the triples of a two-pattern join.
TEST_P(Mvcc, SnapshotsAgreeWithAnOracleAcrossJoinMethods) {
  std::mt19937 random(11);
  std::uniform_int_distribution<int> id(0, 5);
  std::set<std::tuple<int, int, int>> live;
  const std::string query = "SELECT ?a ?b ?x WHERE { ?a <http://ex/p1> ?x . ?b <http://ex/p2> ?x }";
  auto expected = [&] {
    std::vector<std::string> rows;
    for (const auto& [s1, p1, o1] : live) {
      for (const auto& [s2, p2, o2] : live) {
        if (p1 == 1 && p2 == 2 && o1 == o2) {
          rows.push_back("<http://ex/s" + std::to_string(s1) + "> <http://ex/s" +
                         std::to_string(s2) + "> <http://ex/o" + std::to_string(o1) + "> ");
        }
      }
    }
    std::sort(rows.begin(), rows.end());
    return rows;
  };
  for (int round = 0; round < 12; round++) {
    db_.Begin();  // this snapshot stays open through the commits below
    const auto answer = expected();
    for (int commit = 0; commit < 3; commit++) {
      std::thread writer([&] {
        for (int i = 0; i < 8; i++) {
          const auto triple = std::make_tuple(id(random), id(random) % 3 + 1, id(random));
          const auto& [s, p, o] = triple;
          const bool present = live.contains(triple);
          db_.Execute((present ? "DELETE DATA { " : "INSERT DATA { ") + Spell(s, p, o) + " . }",
                      nullptr);
          present ? (void)live.erase(triple) : (void)live.insert(triple);
        }
      });
      writer.join();
      for (const auto join : {JoinMethod::kNestedLoop, JoinMethod::kIndexNestedLoop,
                              JoinMethod::kHash, JoinMethod::kAuto}) {
        auto config = db_.GetConfig();
        config.join = join;
        db_.UpdateConfig(config);
        ASSERT_EQ(Sorted(db_.Execute(query, nullptr)), answer) << "round " << round;
      }
    }
    db_.Commit();
    ASSERT_EQ(Sorted(db_.Execute(query, nullptr)), expected()) << "round " << round;
  }
}

// Writers move one token between two subjects while readers count it. Under snapshot isolation
// every read sees exactly one, and no reader takes a lock.
TEST_P(Mvcc, ReadersNeverSeePartOfACommitAndWritersNeverWaitForThem) {
  db_.Execute("INSERT DATA { " + Spell(1, 1, 1) + " . }", nullptr);
  std::atomic<bool> done{false};
  std::atomic<int> wrong{0};
  std::vector<std::thread> readers;
  for (int i = 0; i < 4; i++) {
    readers.emplace_back([&] {
      while (!done) {
        wrong += db_.Execute("SELECT ?s WHERE { ?s <http://ex/p1> <http://ex/o1> }", nullptr)
                             .rows.size() == 1
                     ? 0
                     : 1;
      }
    });
  }
  bool first = true;
  for (int i = 0; i < 200; i++) {
    db_.Begin();
    db_.Execute("DELETE DATA { " + Spell(first ? 1 : 2, 1, 1) + " . }", nullptr);
    db_.Execute("INSERT DATA { " + Spell(first ? 2 : 1, 1, 1) + " . }", nullptr);
    db_.Commit();
    first = !first;
  }
  done = true;
  for (auto& reader : readers) {
    reader.join();
  }
  EXPECT_EQ(wrong, 0);
}

TEST_P(Mvcc, EntailmentReadsTheSnapshot) {
  const std::string sub = "<http://www.w3.org/2000/01/rdf-schema#subClassOf>";
  const std::string type = "<http://www.w3.org/1999/02/22-rdf-syntax-ns#type>";
  db_.Execute("INSERT DATA { <http://ex/Dog> " + sub + " <http://ex/Animal> . <http://ex/rex> " +
                  type + " <http://ex/Dog> . }",
              nullptr);
  auto config = db_.GetConfig();
  config.entail = true;
  db_.UpdateConfig(config);
  const std::string query = "SELECT ?x WHERE { ?x " + type + " <http://ex/Animal> }";
  db_.Begin();
  EXPECT_EQ(db_.Execute(query, nullptr).rows.size(), 1U);
  std::thread writer([&] {
    db_.Execute("DELETE DATA { <http://ex/Dog> " + sub + " <http://ex/Animal> . }", nullptr);
  });
  writer.join();
  EXPECT_EQ(db_.Execute(query, nullptr).rows.size(), 1U);  // the hierarchy as this snapshot had it
  db_.Commit();
  EXPECT_EQ(db_.Execute(query, nullptr).rows.size(), 0U);
}

INSTANTIATE_TEST_SUITE_P(Backends, Mvcc,
                         ::testing::Values(BackendKind::kMem, BackendKind::kIndexed));

TEST(MvccBackend, HistorySurvivesASwitchOfBackend) {
  Database db;
  db.Execute("INSERT DATA { <http://ex/gone> <http://ex/p> <http://ex/o> . }", nullptr);
  db.Execute("DELETE DATA { <http://ex/gone> <http://ex/p> <http://ex/o> . }", nullptr);
  db.Execute("INSERT DATA { <http://ex/kept> <http://ex/p> <http://ex/o> . }", nullptr);
  db.SetBackend(BackendKind::kIndexed);
  db.SetBackend(BackendKind::kMem);
  db.SetBackend(BackendKind::kIndexed);
  SetIsolation(db, IsolationLevel::kSnapshot);
  EXPECT_EQ(db.GetVersions().Count(1), 1U);
  EXPECT_EQ(db.GetVersions().Count(2), 0U);
  EXPECT_EQ(db.GetVersions().Count(3), 1U);
  const auto at_one = db.GetVersions().Scan({}, 1);
  ASSERT_EQ(at_one.size(), 1U);
  EXPECT_EQ(db.GetDictionary()->Lookup(at_one[0].subject), "<http://ex/gone>");
}

TEST(MvccLocking, OtherLevelsStillLockAndStillCommitVersions) {
  Database db;
  db.Execute("INSERT DATA { <http://ex/a> <http://ex/p> <http://ex/o> . }", nullptr);
  EXPECT_EQ(db.GetVersions().Clock(), 1U);  // read committed writes history too
  db.Begin();
  db.Execute("SELECT ?s WHERE { ?s ?p ?o }", nullptr);
  EXPECT_NE(db.LocksDebug().find("txn"), std::string::npos);
  db.Commit();
}

}  // namespace
}  // namespace ontodb
