#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>

#include "common/database.h"
#include "isolation/spec.h"
#include "shell/session.h"
#include "test_util.h"

namespace ontodb::test {
namespace {

auto SpecDir() -> std::filesystem::path {
  return SourceDir() / "test" / "isolation" / "specs";
}

auto AllSpecs() -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> paths;
  for (const auto& entry : std::filesystem::directory_iterator(SpecDir())) {
    if (entry.path().extension() == ".spec") {
      paths.push_back(entry.path());
    }
  }
  std::sort(paths.begin(), paths.end());
  return paths;
}

void ApplyIsolation(Database* db, const std::string& level) {
  auto config = db->GetConfig();
  if (level == "ru") {
    config.isolation = IsolationLevel::kReadUncommitted;
  } else if (level == "rc") {
    config.isolation = IsolationLevel::kReadCommitted;
  } else if (level == "rr") {
    config.isolation = IsolationLevel::kRepeatableRead;
  } else {
    config.isolation = IsolationLevel::kSerializable;
  }
  db->UpdateConfig(config);
}

class DatabaseDriver : public IsolationDriver {
 public:
  explicit DatabaseDriver(Database& db) : db_(db) {}

  void Submit(int session, const std::string& step, const std::string& text) override {
    pool_.Start(session, [this, text](SessionState& state) {
      if (text == "\\begin") {
        db_.Begin();
        return std::string("ok");
      }
      if (text == "\\commit") {
        db_.Commit();
        return std::string("ok");
      }
      if (text == "\\abort") {
        db_.Abort();
        return std::string("ok");
      }
      const auto result = db_.Execute(text, &state);
      if (result.rows.empty()) {
        return result.message;
      }
      std::string out;
      for (const auto& row : result.rows) {
        if (!out.empty()) {
          out.push_back('\n');
        }
        for (size_t i = 0; i < row.size(); i++) {
          if (i != 0) {
            out.push_back('\t');
          }
          out += row[i];
        }
      }
      return out;
    });
    (void)step;
  }

  auto Poll(int session, std::chrono::milliseconds timeout) -> std::optional<std::string> override {
    return pool_.WaitFor(session, timeout);
  }

 private:
  Database& db_;
  SessionPool pool_;
};

void RunFiles(const std::vector<std::filesystem::path>& paths) {
  ASSERT_FALSE(paths.empty());
  for (const auto& path : paths) {
    const auto spec = LoadSpec(path);
    const auto problem = Validate(spec);
    ASSERT_TRUE(problem.empty()) << path << " " << problem;
    Database db;
    ApplyIsolation(&db, spec.isolation);
    if (!spec.setup.empty()) {
      db.Execute(spec.setup, nullptr);
    }
    DatabaseDriver driver(db);
    for (const auto& perm : spec.permutations) {
      const auto report = RunPermutation(spec, perm, &driver);
      EXPECT_TRUE(report.ok) << path.filename().string() << " " << perm.name << "\n"
                             << report.message;
    }
  }
}

TEST(IsolationHarness, SpecsAreWellFormed) {
  const auto paths = AllSpecs();
  ASSERT_GE(paths.size(), 8u);
  for (const auto& path : paths) {
    const auto spec = LoadSpec(path);
    EXPECT_TRUE(Validate(spec).empty()) << path << " " << Validate(spec);
  }
}

TEST(IsolationHarness, ReportsAMissedBlock) {
  const auto spec = ParseSpec(R"(
name sample
isolation rc
session 1
commit: \commit
session 2
read: SELECT ?s WHERE { ?s <http://ex/p> <http://ex/o> }
permutation wait
2.read
1.commit
expect wait
2.read blocked
1.commit unblocks 2.read
2.read contains <http://ex/b>
)");
  ASSERT_TRUE(Validate(spec).empty()) << Validate(spec);
  ScriptedDriver broken;
  broken.SetOutput("2.read", "<http://ex/b>");
  const auto missed = RunPermutation(spec, spec.permutations[0], &broken);
  EXPECT_FALSE(missed.ok);
  EXPECT_NE(missed.message.find("block"), std::string::npos) << missed.message;

  ScriptedDriver good;
  good.BlockUntil("2.read", "1.commit");
  good.SetOutput("2.read", "row <http://ex/b>");
  const auto matched = RunPermutation(spec, spec.permutations[0], &good);
  EXPECT_TRUE(matched.ok) << matched.message;
}

TEST(P7_4_Isolation, DISABLED_RunsSpecs) {
  RunFiles(AllSpecs());
}

TEST(P7_5_Deadlock, DISABLED_AbortsYounger) {
  std::vector<std::filesystem::path> paths;
  for (const auto& path : AllSpecs()) {
    if (path.filename().string().find("deadlock") != std::string::npos) {
      paths.push_back(path);
    }
  }
  RunFiles(paths);
}

TEST(P7_6_Phantom, DISABLED_RepeatableAllowsSerializableBlocks) {
  std::vector<std::filesystem::path> paths;
  for (const auto& path : AllSpecs()) {
    if (path.filename().string().find("phantom") != std::string::npos) {
      paths.push_back(path);
    }
  }
  RunFiles(paths);
}

}  // namespace
}  // namespace ontodb::test
