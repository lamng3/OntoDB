// PLAN 9: the durable memory database. Each test is one scenario: build some
// state in a directory, drop the Database (or crash it), reopen, and check
// what survived.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "common/crash_point.h"
#include "common/database.h"
#include "common/exception.h"
#include "recovery/log_manager.h"
#include "recovery/snapshot.h"
#include "shell/shell.h"
#include "test_util.h"

namespace ontodb {
namespace {

const std::string kStudents =
    "SELECT ?name WHERE { ?s a <http://example.edu/univ#Student> . "
    "?s <http://example.edu/univ#name> ?name }";
const std::string kMaterialKinds =
    "SELECT ?c WHERE { ?c <http://www.w3.org/2000/01/rdf-schema#subClassOf> "
    "<http://example.org/materials#Material> }";
const std::string kInsertA = "INSERT DATA { <http://ex/a> <http://ex/p> \"one\" . }";
const std::string kInsertB = "INSERT DATA { <http://ex/b> <http://ex/p> \"two\" . }";
const std::string kDeleteA = "DELETE DATA { <http://ex/a> <http://ex/p> \"one\" . }";
const std::string kAllP = "SELECT ?s ?o WHERE { ?s <http://ex/p> ?o }";

auto Rows(Database& db, const std::string& sparql) -> size_t {
  return db.Execute(sparql, nullptr).rows.size();
}

auto Open(const std::filesystem::path& dir, OpenReport* report = nullptr,
          RecoveryMode mode = RecoveryMode::kPointInTime) -> std::unique_ptr<Database> {
  return Database::Open(dir, mode, report);
}

auto ReadBytes(const std::filesystem::path& path) -> std::vector<char> {
  std::ifstream in(path, std::ios::binary);
  return std::vector<char>(std::istreambuf_iterator<char>(in), {});
}

void WriteBytes(const std::filesystem::path& path, const std::vector<char>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// --- 9.1 Record checksums ----------------------------------------------------

TEST(P9_1_Checksum, FlippedByteIsCorruptionShortBufferIsNot) {
  auto record = LogRecord::Term(7, 42, "<http://example.org/materials#Alloy>");
  std::vector<char> buf(record.Size());
  record.SerializeTo(buf.data());
  const auto back = LogRecord::Deserialize(buf.data(), buf.size());
  EXPECT_EQ(back.GetType(), LogRecordType::kTerm);
  EXPECT_EQ(back.GetTermId(), 42u);
  EXPECT_EQ(back.GetTermText(), "<http://example.org/materials#Alloy>");

  auto flipped = buf;
  flipped[buf.size() - 3] ^= 0x01;
  EXPECT_THROW(LogRecord::Deserialize(flipped.data(), flipped.size()), LogCorruptionException);

  try {
    LogRecord::Deserialize(buf.data(), buf.size() - 1);
    FAIL() << "a short buffer must throw";
  } catch (const LogCorruptionException&) {
    FAIL() << "a short buffer is a torn write, not corruption";
  } catch (const StorageException&) {
  }
}

// --- 9.2 Log scanning and reopening a log ------------------------------------

TEST(P9_2_LogScan, StopsAtTornTailAndContinuesLsns) {
  test::TempDir dir;
  const auto path = dir.path() / "wal";
  {
    LogManager log(path.string());
    for (int i = 0; i < 3; i++) {
      auto record = LogRecord::Begin(i + 1);
      log.Append(&record);
    }
    log.Flush(3, true);
  }
  auto bytes = ReadBytes(path);
  bytes.resize(bytes.size() - 5);
  WriteBytes(path, bytes);

  const LogScan scan = ScanLog(path);
  EXPECT_EQ(scan.records.size(), 2u);
  EXPECT_TRUE(scan.torn);
  EXPECT_FALSE(scan.Clean());

  std::filesystem::resize_file(path, scan.valid_bytes);
  LogManager log(path.string());
  auto next = LogRecord::Commit(1);
  EXPECT_EQ(log.Append(&next), 3);
}

// --- 9.3 Durable statements --------------------------------------------------

TEST(P9_3_Durable, TurtleLoadSurvivesReopen) {
  test::TempDir dir;
  {
    auto db = Open(dir.path());
    db->Load(test::DataFile("tiny.ttl").string());
    EXPECT_EQ(Rows(*db, kStudents), 3u);
  }
  OpenReport report;
  auto db = Open(dir.path(), &report);
  EXPECT_EQ(Rows(*db, kStudents), 3u);
  EXPECT_EQ(report.committed, 1u);
  EXPECT_EQ(report.triples, 53u);
}

TEST(P9_3_Durable, OwlRdfXmlLoadSurvivesReopen) {
  test::TempDir dir;
  {
    auto db = Open(dir.path());
    db->Load(test::DataFile("tiny.owl").string());
    EXPECT_EQ(Rows(*db, kMaterialKinds), 2u);
  }
  auto db = Open(dir.path());
  EXPECT_EQ(Rows(*db, kMaterialKinds), 2u);
}

TEST(P9_3_Durable, InsertsAndDeletesSurviveInOrder) {
  test::TempDir dir;
  {
    auto db = Open(dir.path());
    db->Execute(kInsertA, nullptr);
    db->Execute(kInsertB, nullptr);
    db->Execute(kDeleteA, nullptr);
  }
  OpenReport report;
  auto db = Open(dir.path(), &report);
  const auto result = db->Execute(kAllP, nullptr);
  ASSERT_EQ(result.rows.size(), 1u);
  EXPECT_EQ(result.rows[0][0], "<http://ex/b>");
  EXPECT_EQ(report.committed, 3u);
  EXPECT_EQ(report.uncommitted, 0u);
}

TEST(P9_3_Durable, NoOpStatementLogsNothing) {
  test::TempDir dir;
  {
    auto db = Open(dir.path());
    db->Execute(kInsertA, nullptr);
    db->Execute(kInsertA, nullptr);  // already there
  }
  OpenReport report;
  Open(dir.path(), &report);
  EXPECT_EQ(report.committed, 1u);
}

TEST(P9_3_Durable, CrashBeforeCommitLeavesNoTrace) {
  test::TempDir dir;
  {
    auto db = Open(dir.path());
    db->Execute(kInsertA, nullptr);
    CrashPoint::Arm("Durability::Run::before_commit");
    EXPECT_THROW(db->Execute(kInsertB, nullptr), CrashInjected);
    CrashPoint::Disarm();
    EXPECT_EQ(Rows(*db, kAllP), 1u);  // the statement was undone in memory
  }  // the destructor writes the uncommitted records, as a crash might
  OpenReport report;
  auto db = Open(dir.path(), &report);
  EXPECT_EQ(Rows(*db, kAllP), 1u);
  EXPECT_EQ(report.committed, 1u);
  EXPECT_EQ(report.uncommitted, 1u);
  db->Execute(kInsertB, nullptr);
  EXPECT_EQ(Rows(*db, kAllP), 2u);
}

TEST(P9_3_Durable, TermIdsAreStableAcrossReopen) {
  test::TempDir dir;
  std::vector<std::optional<std::string>> before;
  {
    auto db = Open(dir.path());
    db->Load(test::DataFile("pizza.ttl").string());
    db->Execute(kInsertA, nullptr);
    for (term_id_t id = 1; id <= db->GetDictionary()->Size(); id++) {
      before.push_back(db->GetDictionary()->Lookup(id));
    }
  }
  auto db = Open(dir.path());
  ASSERT_EQ(db->GetDictionary()->Size(), before.size());
  for (term_id_t id = 1; id <= before.size(); id++) {
    EXPECT_EQ(db->GetDictionary()->Lookup(id), before[id - 1]);
  }
}

// --- 9.4 Checkpoint ----------------------------------------------------------

TEST(P9_4_Checkpoint, SnapshotThenLogReplaysToSameState) {
  test::TempDir dir;
  {
    auto db = Open(dir.path());
    db->Load(test::DataFile("tiny.ttl").string());
    db->Execute(kInsertA, nullptr);
    db->Checkpoint();
    EXPECT_EQ(std::filesystem::file_size(dir.path() / "wal"), 0u);
    db->Execute(kInsertB, nullptr);
    db->Execute(kDeleteA, nullptr);
  }
  OpenReport report;
  auto db = Open(dir.path(), &report);
  EXPECT_TRUE(report.had_snapshot);
  EXPECT_EQ(report.snapshot_triples, 54u);
  EXPECT_EQ(report.committed, 2u);
  EXPECT_EQ(Rows(*db, kStudents), 3u);
  const auto result = db->Execute(kAllP, nullptr);
  ASSERT_EQ(result.rows.size(), 1u);
  EXPECT_EQ(result.rows[0][0], "<http://ex/b>");
}

TEST(P9_4_Checkpoint, CrashAfterSnapshotDoesNotReplayTwice) {
  test::TempDir dir;
  {
    auto db = Open(dir.path());
    db->Execute(kInsertA, nullptr);
    db->Execute(kDeleteA, nullptr);
    db->Execute(kInsertA, nullptr);
    CrashPoint::Arm("Durability::Checkpoint::after_snapshot");
    EXPECT_THROW(db->Checkpoint(), CrashInjected);
    CrashPoint::Disarm();
  }
  EXPECT_GT(std::filesystem::file_size(dir.path() / "wal"), 0u);  // old log still there
  OpenReport report;
  auto db = Open(dir.path(), &report);
  EXPECT_EQ(report.log_records, 0u);  // every record is at or below the snapshot
  EXPECT_EQ(Rows(*db, kAllP), 1u);
  db->Execute(kInsertB, nullptr);
  db.reset();
  auto reopened = Open(dir.path());
  EXPECT_EQ(Rows(*reopened, kAllP), 2u);
}

TEST(P9_4_Checkpoint, CorruptSnapshotRefusesToOpen) {
  test::TempDir dir;
  {
    auto db = Open(dir.path());
    db->Execute(kInsertA, nullptr);
    db->Checkpoint();
  }
  auto bytes = ReadBytes(dir.path() / "snapshot");
  bytes[bytes.size() - 2] ^= 0x01;
  WriteBytes(dir.path() / "snapshot", bytes);
  EXPECT_THROW(Open(dir.path()), SnapshotCorruptionException);
}

// --- 9.5 Recovery modes ------------------------------------------------------

TEST(P9_5_RecoveryMode, TornTailKeepsEarlierCommits) {
  test::TempDir dir;
  {
    auto db = Open(dir.path());
    db->Execute(kInsertA, nullptr);
    db->Execute(kInsertB, nullptr);
  }
  const auto wal = dir.path() / "wal";
  auto bytes = ReadBytes(wal);
  bytes.resize(bytes.size() - 4);  // tear the last commit record
  WriteBytes(wal, bytes);

  EXPECT_THROW(Open(dir.path(), nullptr, RecoveryMode::kAbsoluteConsistency),
               LogCorruptionException);

  OpenReport report;
  auto db = Open(dir.path(), &report);
  EXPECT_FALSE(report.stop.empty());
  EXPECT_EQ(report.committed, 1u);
  EXPECT_EQ(Rows(*db, kAllP), 1u);
  db->Execute(kInsertB, nullptr);  // appends after the valid prefix
  db.reset();

  OpenReport clean;
  auto again = Open(dir.path(), &clean, RecoveryMode::kAbsoluteConsistency);
  EXPECT_TRUE(clean.stop.empty());
  EXPECT_EQ(Rows(*again, kAllP), 2u);
}

TEST(P9_5_RecoveryMode, CorruptRecordDropsEverythingAfterIt) {
  test::TempDir dir;
  {
    auto db = Open(dir.path());
    db->Execute(kInsertA, nullptr);
  }
  const auto first = std::filesystem::file_size(dir.path() / "wal");
  {
    auto db = Open(dir.path());
    db->Execute(kInsertB, nullptr);
  }
  const auto wal = dir.path() / "wal";
  auto bytes = ReadBytes(wal);
  bytes[first + 10] ^= 0x40;  // inside the second transaction's begin record
  WriteBytes(wal, bytes);

  OpenReport report;
  auto db = Open(dir.path(), &report);
  EXPECT_NE(report.stop.find("checksum"), std::string::npos);
  EXPECT_GT(report.discarded_bytes, 0u);
  const auto result = db->Execute(kAllP, nullptr);
  ASSERT_EQ(result.rows.size(), 1u);
  EXPECT_EQ(result.rows[0][0], "<http://ex/a>");
}

// --- 9.6 Verify --------------------------------------------------------------

TEST(P9_6_Verify, CleanDatabaseVerifies) {
  test::TempDir dir;
  auto db = Open(dir.path());
  db->Load(test::DataFile("tiny.owl").string());
  db->Checkpoint();
  db->Execute(kInsertA, nullptr);
  const auto report = db->Verify();
  EXPECT_NE(report.find("verify: ok"), std::string::npos) << report;
}

TEST(P9_6_Verify, TamperedLogFailsVerify) {
  test::TempDir dir;
  auto db = Open(dir.path());
  db->Execute(kInsertA, nullptr);
  {
    std::ofstream out(dir.path() / "wal", std::ios::binary | std::ios::app);
    out << "not a log record, just bytes that someone appended";
  }
  const auto report = db->Verify();
  EXPECT_NE(report.find("wal: FAILED"), std::string::npos) << report;
  EXPECT_NE(report.find("verify: FAILED"), std::string::npos);
}

TEST(P9_6_Verify, MemoryDatabaseHasNothingToVerify) {
  Database db;
  EXPECT_NE(db.Verify().find("nothing on disk"), std::string::npos);
}

// --- 9.7 A whole shell session -----------------------------------------------

auto RunShell(Database& db, const std::string& script) -> std::string {
  std::istringstream in(script);
  std::ostringstream out;
  Shell shell(db, in, out);
  shell.Run();
  return out.str();
}

TEST(P9_7_ShellFlow, LoadUpdateCheckpointVerifyReopen) {
  test::TempDir dir;
  {
    auto db = Open(dir.path());
    const auto out = RunShell(*db, "\\load " + test::DataFile("tiny.owl").string() + "\n" +
                                       "INSERT DATA { <http://example.org/materials#Ceramic> "
                                       "<http://www.w3.org/2000/01/rdf-schema#subClassOf> "
                                       "<http://example.org/materials#Material> . }\n"
                                       "\\checkpoint\n"
                                       "DELETE DATA { <http://example.org/materials#Polymer> "
                                       "<http://www.w3.org/2000/01/rdf-schema#subClassOf> "
                                       "<http://example.org/materials#Material> . }\n"
                                       "\\verify\n\\log 2\n\\quit\n");
    EXPECT_NE(out.find("loaded 14 triples"), std::string::npos) << out;
    EXPECT_NE(out.find("checkpoint at lsn"), std::string::npos) << out;
    EXPECT_NE(out.find("verify: ok"), std::string::npos) << out;
    EXPECT_NE(out.find("commit"), std::string::npos) << out;
  }
  auto db = Open(dir.path());
  const auto out = RunShell(*db, kMaterialKinds + "\n\\quit\n");
  EXPECT_NE(out.find("Ceramic"), std::string::npos) << out;
  EXPECT_NE(out.find("Metal"), std::string::npos) << out;
  EXPECT_EQ(out.find("Polymer"), std::string::npos) << out;
}

}  // namespace
}  // namespace ontodb
