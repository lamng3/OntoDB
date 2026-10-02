#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <thread>
#include <vector>

#include "catalog/catalog.h"
#include "common/exception.h"
#include "concurrency/lock_manager.h"
#include "concurrency/transaction_manager.h"
#include "recovery/checkpoint_manager.h"
#include "recovery/log_manager.h"
#include "recovery/log_recovery.h"
#include "storage/env.h"
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

TEST(P8_1_LogRecord, DISABLED_RoundTrip) {
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

TEST(P8_2_LogManager, DISABLED_GroupCommit) {
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

TEST(P8_4_Undo, DISABLED_AbortWritesClrAndDropsTriple) {
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

TEST(P8_5_Checkpoint, DISABLED_RecordsDurableLsn) {
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

TEST(P8_6_Recovery, DISABLED_AnalysisSplitsWinnersAndLosers) {
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

TEST(P8_7_CrashTest, DISABLED_RestartDropsUncommitted) {
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

}  // namespace
}  // namespace ontodb
