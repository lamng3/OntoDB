#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <set>
#include <thread>
#include <vector>

#include "catalog/catalog.h"
#include "common/crash_point.h"
#include "common/exception.h"
#include "concurrency/lock_manager.h"
#include "concurrency/transaction_manager.h"
#include "recovery/checkpoint_manager.h"
#include "recovery/log_manager.h"
#include "recovery/log_recovery.h"
#include "storage/env.h"
#include "storage/index/b_plus_tree.h"
#include "storage/page/b_plus_tree_header_page.h"
#include "store/mem_store.h"

namespace ontodb {
namespace {

auto ReadFile(const std::filesystem::path& path) -> std::vector<char> {
  std::ifstream in(path, std::ios::binary);
  return std::vector<char>(std::istreambuf_iterator<char>(in), {});
}

auto Records(const std::vector<char>& bytes) -> std::vector<LogRecord> {
  std::vector<LogRecord> records;
  size_t off = 0;
  while (off < bytes.size()) {
    auto record = LogRecord::Deserialize(bytes.data() + off, bytes.size() - off);
    const auto size = record.Size();
    if (size == 0 || off + size > bytes.size()) {
      break;
    }
    off += size;
    records.push_back(std::move(record));
    if (records.size() > 10000) {
      break;
    }
  }
  return records;
}

TEST(P8_1_LogRecord, RoundTrip) {
  const Triple triple{4, 5, 6};
  std::vector<LogRecord> records = {
      LogRecord::Begin(3),
      LogRecord::Commit(3),
      LogRecord::Abort(3),
      LogRecord::Update(3, 9, true, triple),
      LogRecord::Clr(3, 9, false, triple, 12),
      LogRecord::Checkpoint({1, 2, 3}),
  };
  for (auto record : records) {
    std::vector<char> buf(record.Size());
    record.SerializeTo(buf.data());
    const auto back = LogRecord::Deserialize(buf.data(), buf.size());
    EXPECT_EQ(back.GetType(), record.GetType());
    EXPECT_EQ(back.GetTxnId(), record.GetTxnId());
    EXPECT_THROW(LogRecord::Deserialize(buf.data(), 1), StorageException);
  }
  const auto update = LogRecord::Update(3, 9, true, triple);
  std::vector<char> buf(update.Size());
  update.SerializeTo(buf.data());
  const auto back = LogRecord::Deserialize(buf.data(), buf.size());
  EXPECT_EQ(back.GetPageId(), 9);
  EXPECT_TRUE(back.IsInsert());
  EXPECT_EQ(back.GetTriple(), triple);
  const auto clr = LogRecord::Clr(3, 9, false, triple, 12);
  std::vector<char> clr_buf(clr.Size());
  clr.SerializeTo(clr_buf.data());
  EXPECT_EQ(LogRecord::Deserialize(clr_buf.data(), clr_buf.size()).GetUndoNext(), 12);
  const auto checkpoint = LogRecord::Checkpoint({1, 2, 3});
  std::vector<char> cp_buf(checkpoint.Size());
  checkpoint.SerializeTo(cp_buf.data());
  EXPECT_EQ(LogRecord::Deserialize(cp_buf.data(), cp_buf.size()).GetActiveTxns(),
            (std::vector<txn_id_t>{1, 2, 3}));
}

TEST(P8_2_LogManager, GroupCommit) {
  test::TempDir dir;
  const auto path = (dir.path() / "wal").string();
  IoStats stats;
  LogManager log(path, &stats);
  log.StartFlushThread();
  constexpr int kThreads = 4;
  constexpr int kEach = 20;
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; t++) {
    threads.emplace_back([&] {
      ready.fetch_add(1);
      while (!go.load()) {
      }
      for (int i = 0; i < kEach; i++) {
        auto record = LogRecord::Commit(1);
        const auto lsn = log.Append(&record);
        log.Flush(lsn, true);
      }
    });
  }
  while (ready.load() < kThreads) {
  }
  go.store(true);
  for (auto& thread : threads) {
    thread.join();
  }
  EXPECT_LT(log.GetFlushCount(), static_cast<uint64_t>(kThreads * kEach));
  EXPECT_GE(stats.log_flushes(), 1u);
  EXPECT_GE(log.GetPersistentLsn(), 1);
  log.StopFlushThread();
  EXPECT_FALSE(Records(ReadFile(path)).empty());
}

TEST(P8_4_Undo, AbortWritesClrAndDropsTriple) {
  test::TempDir dir;
  LogManager log((dir.path() / "wal").string());
  log.StartFlushThread();
  MemStore store;
  LockManager locks;
  TransactionManager txns(&locks, &store, &log);
  auto* txn = txns.Begin(IsolationLevel::kReadCommitted);
  ASSERT_NE(txn, nullptr);
  const Triple triple{1, 2, 3};
  auto record = LogRecord::Update(txn->GetId(), 1, true, triple);
  const auto lsn = log.Append(&record);
  log.Flush(lsn, true);
  store.Insert(triple);
  txns.Abort(txn);
  auto iter = store.Scan({});
  iter->Init();
  Triple found;
  EXPECT_FALSE(iter->Next(&found));
  log.StopFlushThread();
  bool saw_clr = false;
  for (const auto& item : Records(ReadFile(dir.path() / "wal"))) {
    if (item.GetType() == LogRecordType::kClr) {
      saw_clr = true;
    }
  }
  EXPECT_TRUE(saw_clr);
}

TEST(P8_5_Checkpoint, RecordsDurableLsn) {
  test::TempDir dir;
  LogManager log((dir.path() / "wal").string());
  log.StartFlushThread();
  test::DiskEnv env;
  LockManager locks;
  MemStore store;
  TransactionManager txns(&locks, &store, &log);
  CheckpointManager checkpoints(&log, &env.bpm, &txns);
  checkpoints.Checkpoint();
  const auto lsn = checkpoints.LastCheckpointLsn();
  EXPECT_NE(lsn, INVALID_LSN);
  log.Flush(lsn, true);
  log.StopFlushThread();
  bool saw = false;
  for (const auto& item : Records(ReadFile(dir.path() / "wal"))) {
    if (item.GetType() == LogRecordType::kCheckpoint) {
      saw = true;
    }
  }
  EXPECT_TRUE(saw);
}

TEST(P8_6_Recovery, AnalysisSplitsWinnersAndLosers) {
  test::TempDir dir;
  const auto db_path = (dir.path() / "db").string();
  const auto log_path = (dir.path() / "wal").string();
  {
    LogManager log(log_path);
    log.StartFlushThread();
    auto begin = LogRecord::Begin(1);
    auto update = LogRecord::Update(1, 1, true, Triple{1, 2, 3});
    auto commit = LogRecord::Commit(1);
    auto loser = LogRecord::Begin(2);
    auto loser_update = LogRecord::Update(2, 1, true, Triple{4, 5, 6});
    log.Append(&begin);
    log.Append(&update);
    log.Append(&commit);
    log.Append(&loser);
    const auto lsn = log.Append(&loser_update);
    log.Flush(lsn, true);
    log.StopFlushThread();
  }
  DiskManager disk(db_path);
  BufferPoolManager bpm(8, &disk, 2);
  Catalog catalog(&bpm);
  LogManager log(log_path);
  LogRecovery recovery(&log, &bpm, &disk, &catalog);
  recovery.Analyze();
  const auto winners = recovery.WinnerTxns();
  const auto losers = recovery.LoserTxns();
  EXPECT_NE(std::find(winners.begin(), winners.end(), 1), winners.end());
  EXPECT_NE(std::find(losers.begin(), losers.end(), 2), losers.end());
  recovery.Redo();
  recovery.Undo();
  const auto summary = recovery.Summary();
  EXPECT_NE(summary.find("analysis"), std::string::npos);
  EXPECT_NE(summary.find("redo"), std::string::npos);
  EXPECT_NE(summary.find("undo"), std::string::npos);
  EXPECT_EQ(bpm.GetPinnedFrameCount(), 0u);
}

TEST(P8_7_CrashTest, RestartDropsUncommitted) {
  test::TempDir dir;
  const auto db_path = (dir.path() / "db").string();
  const auto log_path = (dir.path() / "wal").string();
  {
    DiskManager disk(db_path);
    BufferPoolManager bpm(8, &disk, 2);
    Catalog catalog(&bpm);
    LogManager log(log_path);
    log.StartFlushThread();
    LockManager locks;
    MemStore store;
    TransactionManager txns(&locks, &store, &log);
    auto* committed = txns.Begin(IsolationLevel::kReadCommitted);
    auto kept = LogRecord::Update(committed->GetId(), 1, true, Triple{1, 2, 3});
    log.Append(&kept);
    txns.Commit(committed);
    auto* open = txns.Begin(IsolationLevel::kReadCommitted);
    auto dropped = LogRecord::Update(open->GetId(), 1, true, Triple{4, 5, 6});
    log.Append(&dropped);
    log.StopFlushThread();
    bpm.FlushAllPages();
    disk.ShutDown();
  }
  DiskManager disk(db_path);
  BufferPoolManager bpm(8, &disk, 2);
  Catalog catalog(&bpm);
  LogManager log(log_path);
  LogRecovery recovery(&log, &bpm, &disk, &catalog);
  recovery.Analyze();
  recovery.Redo();
  recovery.Undo();
  EXPECT_NE(std::find(recovery.WinnerTxns().begin(), recovery.WinnerTxns().end(), 1),
            recovery.WinnerTxns().end());
  EXPECT_NE(std::find(recovery.LoserTxns().begin(), recovery.LoserTxns().end(), 2),
            recovery.LoserTxns().end());
}

// A crash in the middle of a split or merge, with the log flushed as far as
// it may be and the files copied at that instant. After redo, the tree must
// be whole and hold either every key from before the operation or every key
// from after it: a structure change reaches the log all at once or not at all.
struct MidOperationCrash {
  std::string point;
  int occurrence;
};

class P8_3_StructureChange : public ::testing::TestWithParam<MidOperationCrash> {};

TEST_P(P8_3_StructureChange, IsAllOrNothingAfterRedo) {
  test::TempDir dir;
  const auto live = dir.path() / "live";
  const auto copy = dir.path() / "crash";
  std::filesystem::create_directories(live);
  std::filesystem::create_directories(copy);
  std::vector<int> order(80);
  for (int i = 0; i < 80; i++) {
    order[static_cast<size_t>(i)] = (i * 37) % 80;
  }
  std::set<int> before;
  std::set<int> after;
  bool captured = false;
  page_id_t header_id = INVALID_PAGE_ID;
  {
    DiskManager disk((live / "db").string());
    LogManager log((live / "wal").string());
    log.StartFlushThread();
    BufferPoolManager bpm(16, &disk, 2, &log);
    {
      WritePageGuard header = bpm.NewPageGuarded(&header_id);
      BPlusTreeHeaderPage page(header.GetPage());
      page.Init();
      page.SetRootPageId(INVALID_PAGE_ID);
    }
    BPlusTree tree("spo", header_id, &bpm, nullptr, 4, 4);
    std::set<int> current;
    int seen = 0;
    int in_flight = -1;
    bool inserting = true;
    CrashPoint::SetProbe([&](std::string_view name) {
      if (captured || name != GetParam().point || ++seen != GetParam().occurrence) {
        return;
      }
      log.FlushAvailable();
      std::filesystem::copy_file(live / "db", copy / "db");
      std::filesystem::copy_file(live / "wal", copy / "wal");
      before = current;
      after = current;
      if (inserting) {
        after.insert(in_flight);
      } else {
        after.erase(in_flight);
      }
      captured = true;
    });
    for (const int k : order) {
      in_flight = k;
      tree.Insert(TripleKey::Encode(1, 1, static_cast<term_id_t>(k)));
      current.insert(k);
    }
    inserting = false;
    for (const int k : order) {
      in_flight = (k * 7) % 80;
      tree.Remove(TripleKey::Encode(1, 1, static_cast<term_id_t>(in_flight)));
      current.erase(in_flight);
    }
    CrashPoint::Disarm();
    log.StopFlushThread();
  }
  ASSERT_TRUE(captured) << "the crash point was never reached";

  DiskManager disk((copy / "db").string());
  LogManager log((copy / "wal").string());
  BufferPoolManager bpm(16, &disk, 2, &log);
  LogRecovery recovery(&log, &bpm, &disk, nullptr);
  recovery.Analyze();
  recovery.Redo();
  BPlusTree tree("spo", header_id, &bpm, nullptr, 4, 4);
  const auto broken = tree.CheckInvariants();
  ASSERT_FALSE(broken.has_value()) << *broken;
  std::set<int> scanned;
  for (auto it = tree.Begin(); !it.IsEnd(); ++it) {
    term_id_t s = 0;
    term_id_t p = 0;
    term_id_t o = 0;
    (*it).Decode(&s, &p, &o);
    scanned.insert(static_cast<int>(o));
  }
  EXPECT_TRUE(scanned == before || scanned == after)
      << "scan found " << scanned.size() << " keys; expected " << before.size() << " or "
      << after.size();
  for (int k = 0; k < 80; k++) {
    EXPECT_EQ(tree.GetValue(TripleKey::Encode(1, 1, static_cast<term_id_t>(k))),
              scanned.contains(k))
        << "lookup and scan disagree on " << k;
  }
}

INSTANTIATE_TEST_SUITE_P(
    CrashPoints, P8_3_StructureChange,
    ::testing::Values(MidOperationCrash{"BPlusTree::Insert::after_leaf_split", 1},
                      MidOperationCrash{"BPlusTree::Insert::after_leaf_split", 9},
                      MidOperationCrash{"BPlusTree::Insert::after_internal_split", 1},
                      MidOperationCrash{"BPlusTree::Insert::after_internal_split", 3},
                      MidOperationCrash{"BPlusTree::Remove::after_redistribute", 2},
                      MidOperationCrash{"BPlusTree::Remove::after_merge", 1},
                      MidOperationCrash{"BPlusTree::Remove::after_merge", 6}),
    [](const ::testing::TestParamInfo<MidOperationCrash>& info) {
      std::string name = info.param.point.substr(info.param.point.rfind(':') + 1);
      return name + "_" + std::to_string(info.param.occurrence);
    });

}  // namespace
}  // namespace ontodb
