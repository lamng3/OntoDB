// MVCC on disk: history, versions, and transactions across a reopen, a checkpoint, and a crash.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>

#include "common/crash_point.h"
#include "common/crc32c.h"
#include "common/database.h"
#include "common/exception.h"
#include "recovery/snapshot.h"
#include "test_util.h"

namespace ontodb {
namespace {

const std::string kA = "<http://ex/a> <http://ex/p> <http://ex/o>";
const std::string kB = "<http://ex/b> <http://ex/p> <http://ex/o>";
const std::string kC = "<http://ex/c> <http://ex/p> <http://ex/o>";

void Insert(Database& db, const std::string& triple) {
  db.Execute("INSERT DATA { " + triple + " . }", nullptr);
}

void Delete(Database& db, const std::string& triple) {
  db.Execute("DELETE DATA { " + triple + " . }", nullptr);
}

// A database with three commits and two versions, whichever way it comes back.
void Build(Database& db) {
  Insert(db, kA);
  db.Tag("0.1.0");
  Insert(db, kB);
  Delete(db, kA);
  db.Tag("0.2.0");
}

void ExpectBuilt(const Database& db) {
  const auto& versions = db.GetVersions();
  EXPECT_EQ(versions.Clock(), 3U);
  EXPECT_EQ(versions.FindTag("0.1.0"), 1U);
  EXPECT_EQ(versions.FindTag("0.2.0"), 3U);
  EXPECT_EQ(versions.Count(1), 1U);
  EXPECT_EQ(versions.Count(2), 2U);
  EXPECT_EQ(versions.Count(3), 1U);
}

auto Verified(Database& db) -> bool {
  return db.Verify().find("verify: ok") != std::string::npos;
}

TEST(MvccDurable, HistoryAndVersionsSurviveAReopen) {
  test::TempDir dir;
  {
    auto db = Database::Open(dir.path());
    Build(*db);
    EXPECT_TRUE(Verified(*db));
  }
  auto db = Database::Open(dir.path());
  ExpectBuilt(*db);
  EXPECT_TRUE(Verified(*db));
}

TEST(MvccDurable, HistoryAndVersionsSurviveACheckpoint) {
  test::TempDir dir;
  {
    auto db = Database::Open(dir.path());
    Build(*db);
    db->Checkpoint();
    EXPECT_TRUE(Verified(*db));
  }
  {
    auto db = Database::Open(dir.path());
    ExpectBuilt(*db);
    Insert(*db, kC);  // the clock goes on from where it was
    db->Tag("0.3.0");
    EXPECT_EQ(db->GetVersions().FindTag("0.3.0"), 4U);
    EXPECT_TRUE(Verified(*db));
  }
  auto db = Database::Open(dir.path());
  EXPECT_EQ(db->GetVersions().FindTag("0.2.0"), 3U);
  EXPECT_EQ(db->GetVersions().FindTag("0.3.0"), 4U);
  EXPECT_EQ(db->GetVersions().Count(4), 2U);
}

TEST(MvccDurable, ACrashBetweenSnapshotAndLogResetReplaysNothingTwice) {
  test::TempDir dir;
  {
    auto db = Database::Open(dir.path());
    Build(*db);
    CrashPoint::Arm("Durability::Checkpoint::after_snapshot");
    EXPECT_THROW(db->Checkpoint(), CrashInjected);
    CrashPoint::Disarm();
  }
  auto db = Database::Open(dir.path());
  ExpectBuilt(*db);
  EXPECT_TRUE(Verified(*db));
}

TEST(MvccDurable, ACommitThatCannotBeLoggedDidNotHappen) {
  test::TempDir dir;
  auto db = Database::Open(dir.path());
  Insert(*db, kA);
  CrashPoint::Arm("Durability::Run::before_commit");
  EXPECT_THROW(Insert(*db, kB), CrashInjected);
  CrashPoint::Disarm();
  EXPECT_EQ(db->GetVersions().Clock(), 1U);
  EXPECT_EQ(db->Execute("SELECT ?s WHERE { ?s <http://ex/p> ?o }", nullptr).rows.size(), 1U);
}

TEST(MvccDurable, ExplicitTransactionsReachTheLogWhole) {
  test::TempDir dir;
  {
    auto db = Database::Open(dir.path());
    db->Begin();
    Insert(*db, kA);
    Insert(*db, kB);
    db->Commit();
    db->Begin();
    Insert(*db, kC);
    db->Abort();
    db->Begin();
    Delete(*db, kA);
    // Still open when the database goes away: as a crash would leave it.
  }
  auto db = Database::Open(dir.path());
  EXPECT_EQ(db->GetVersions().Clock(), 1U);
  EXPECT_EQ(db->GetVersions().Count(1), 2U);
  EXPECT_EQ(db->Execute("SELECT ?s WHERE { ?s <http://ex/p> ?o }", nullptr).rows.size(), 2U);
  EXPECT_TRUE(Verified(*db));
}

TEST(MvccDurable, ACheckpointDoesNotCatchAnOpenTransaction) {
  test::TempDir dir;
  {
    auto db = Database::Open(dir.path());
    Insert(*db, kA);
    db->Begin();
    Insert(*db, kB);
    Delete(*db, kA);
    db->Checkpoint();
    EXPECT_TRUE(Verified(*db));  // the live store differs, and that is not damage
    db->Abort();
    EXPECT_TRUE(Verified(*db));
  }
  auto db = Database::Open(dir.path());
  const auto rows = db->Execute("SELECT ?s WHERE { ?s <http://ex/p> ?o }", nullptr).rows;
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows[0][0], "<http://ex/a>");
}

TEST(MvccDurable, ASnapshotOfTheOldFormatReadsAsOneCommit) {
  test::TempDir dir;
  {
    auto db = Database::Open(dir.path());
    Insert(*db, kA);
    db->Checkpoint();
  }
  const auto path = dir.path() / "snapshot";
  const Snapshot now = ReadSnapshot(path);
  std::string body;
  for (const auto& term : now.terms) {
    body += "T\t" + term + "\n";
  }
  const auto& t = now.triples.at(0);
  body += "S " + std::to_string(t.subject) + " " + std::to_string(t.predicate) + " " +
          std::to_string(t.object) + "\n";
  // The version 1 checksum, computed the way the format says.
  char crc[9];
  std::snprintf(crc, sizeof(crc), "%08x", Crc32c(body.data(), body.size()));
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "ontodb-snapshot 1\nlsn " << now.lsn << "\ntxn " << now.next_txn << "\nterms "
        << now.terms.size() << "\ntriples 1\ncrc32c " << crc << "\n\n"
        << body;
  }
  auto db = Database::Open(dir.path());
  EXPECT_EQ(db->GetVersions().Clock(), 1U);
  EXPECT_EQ(db->GetVersions().Count(1), 1U);
  Insert(*db, kB);
  EXPECT_EQ(db->GetVersions().Clock(), 2U);
  EXPECT_TRUE(Verified(*db));
}

TEST(MvccDurable, ATornLogKeepsEveryVersionBeforeTheTear) {
  test::TempDir dir;
  {
    auto db = Database::Open(dir.path());
    Build(*db);
    Insert(*db, kC);
    db->Tag("0.3.0");
  }
  const auto wal = dir.path() / "wal";
  std::filesystem::resize_file(wal, std::filesystem::file_size(wal) - 3);  // inside the last tag
  auto db = Database::Open(dir.path());
  EXPECT_EQ(db->GetVersions().Clock(), 4U);  // the commit before the tag was whole
  EXPECT_EQ(db->GetVersions().FindTag("0.2.0"), 3U);
  EXPECT_EQ(db->GetVersions().FindTag("0.3.0"), std::nullopt);
  EXPECT_EQ(db->Tag("0.3.0"), 4U);  // and naming it again is what a retry does
}

TEST(MvccDurable, OneWriterAtATime) {
  test::TempDir dir;
  auto first = Database::Open(dir.path());
  EXPECT_THROW(Database::Open(dir.path()), StorageException);
  first.reset();
  EXPECT_NO_THROW(Database::Open(dir.path()));
}

TEST(MvccDurable, AReaderOpensBesideAWriterAndSeesWhatWasCommitted) {
  test::TempDir dir;
  auto writer = Database::Open(dir.path());
  Build(*writer);
  writer->Begin();
  Insert(*writer, kC);  // not committed, so not on disk
  {
    auto reader = Database::Open(dir.path(), RecoveryMode::kPointInTime, nullptr, true);
    ExpectBuilt(*reader);
    EXPECT_TRUE(reader->IsReadOnly());
    reader->SetAt("0.1.0");
    EXPECT_EQ(reader->Execute("SELECT ?s WHERE { ?s <http://ex/p> ?o }", nullptr).rows.size(), 1U);
  }
  writer->Commit();
  writer->Tag("0.3.0");
  EXPECT_TRUE(Verified(*writer));  // the reader left the writer's files as they were
  EXPECT_EQ(writer->GetVersions().FindTag("0.3.0"), 4U);
}

TEST(MvccDurable, AReadOnlyDatabaseRefusesEveryWrite) {
  test::TempDir dir;
  Build(*Database::Open(dir.path()));
  auto reader = Database::Open(dir.path(), RecoveryMode::kPointInTime, nullptr, true);
  EXPECT_THROW(Insert(*reader, kC), std::runtime_error);
  EXPECT_THROW(reader->Tag("x"), std::runtime_error);
  EXPECT_THROW(reader->Load("/nowhere.nt"), std::runtime_error);
  EXPECT_THROW(reader->Replace("/nowhere.nt"), std::runtime_error);
  EXPECT_THROW(reader->Checkpoint(), std::runtime_error);
  ExpectBuilt(*reader);
}

TEST(MvccDurable, AReaderOfATornLogCutsNothing) {
  test::TempDir dir;
  {
    auto db = Database::Open(dir.path());
    Build(*db);
  }
  const auto wal = dir.path() / "wal";
  const auto size = std::filesystem::file_size(wal);
  std::filesystem::resize_file(wal, size - 3);
  OpenReport report;
  auto reader = Database::Open(dir.path(), RecoveryMode::kPointInTime, &report, true);
  EXPECT_FALSE(report.stop.empty());
  EXPECT_EQ(std::filesystem::file_size(wal), size - 3);
  EXPECT_EQ(reader->GetVersions().Clock(), 3U);
  EXPECT_EQ(reader->GetVersions().FindTag("0.2.0"), std::nullopt);
}

TEST(MvccDurable, AReaderOfNothingSaysSo) {
  EXPECT_THROW(Database::Open("/no/such/ontodb", RecoveryMode::kPointInTime, nullptr, true),
               StorageException);
}

TEST(MvccDurable, ATagCannotMoveEvenAfterAReopen) {
  test::TempDir dir;
  {
    auto db = Database::Open(dir.path());
    Insert(*db, kA);
    db->Tag("0.1.0");
  }
  auto db = Database::Open(dir.path());
  EXPECT_EQ(db->Tag("0.1.0"), 1U);  // asked again: no change
  Insert(*db, kB);
  EXPECT_THROW(db->Tag("0.1.0"), std::invalid_argument);
  EXPECT_THROW(db->Tag("two words"), std::invalid_argument);
}

}  // namespace
}  // namespace ontodb
