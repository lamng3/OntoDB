#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "concurrency/lock_manager.h"
#include "concurrency/transaction_manager.h"
#include "storage/env.h"
#include "storage/index/b_plus_tree.h"
#include "storage/page/b_plus_tree_header_page.h"
#include "store/mem_store.h"

namespace ontodb {
namespace {

auto KeyResource(const TripleKey& key) -> LockResource {
  LockResource resource;
  resource.type = ResourceType::kKey;
  resource.name = "spo";
  resource.key = key;
  return resource;
}

TEST(P7_1_Crabbing, DISABLED_ConcurrentInsertDeleteScan) {
  test::DiskEnv env(32, 2);
  page_id_t header_id = INVALID_PAGE_ID;
  Page* page = env.bpm.NewPage(&header_id);
  ASSERT_NE(page, nullptr);
  BPlusTreeHeaderPage header(page);
  header.Init();
  header.SetRootPageId(INVALID_PAGE_ID);
  ASSERT_TRUE(env.bpm.UnpinPage(header_id, true));
  BPlusTree tree("spo", header_id, &env.bpm);
  ASSERT_TRUE(tree.Insert(TripleKey::Encode(1, 1, 1)));
  ASSERT_TRUE(tree.Remove(TripleKey::Encode(1, 1, 1)));
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; t++) {
    threads.emplace_back([&tree, t] {
      for (int i = 0; i < 50; i++) {
        const auto key =
            TripleKey::Encode(static_cast<term_id_t>(t + 1), 1, static_cast<term_id_t>(i + 1));
        tree.Insert(key);
        if ((i % 2) == 0) {
          tree.Remove(key);
        }
        auto it = tree.Begin(key, 3);
        if (!it.IsEnd()) {
          (void)*it;
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  EXPECT_FALSE(tree.CheckInvariants().has_value());
  EXPECT_EQ(env.bpm.GetPinnedFrameCount(), 0u);
}

TEST(P7_2_Transaction, DISABLED_AbortUndoesWriteSet) {
  MemStore store;
  LockManager locks;
  TransactionManager txns(&locks, &store);
  auto* txn = txns.Begin(IsolationLevel::kReadCommitted);
  ASSERT_NE(txn, nullptr);
  EXPECT_EQ(txn->GetState(), TransactionState::kGrowing);
  store.Insert({1, 2, 3});
  txn->GetWriteSet().push_back({true, {1, 2, 3}});
  store.Insert({4, 5, 6});
  txn->GetWriteSet().push_back({true, {4, 5, 6}});
  txns.Abort(txn);
  EXPECT_EQ(txn->GetState(), TransactionState::kAborted);
  auto iter = store.Scan({});
  iter->Init();
  Triple triple;
  EXPECT_FALSE(iter->Next(&triple));
  auto* committed = txns.Begin(IsolationLevel::kReadCommitted);
  store.Insert({7, 8, 9});
  committed->GetWriteSet().push_back({true, {7, 8, 9}});
  txns.Commit(committed);
  EXPECT_EQ(committed->GetState(), TransactionState::kCommitted);
  const auto debug = txns.DebugString();
  EXPECT_NE(debug.find(std::to_string(committed->GetId())), std::string::npos);
}

TEST(P7_3_LockManager, DISABLED_ExclusiveBlocksAndSharedCompatible) {
  LockManager locks;
  Transaction first(1, IsolationLevel::kReadCommitted);
  const auto key = KeyResource(TripleKey{});
  LockResource database;
  database.type = ResourceType::kDatabase;
  ASSERT_TRUE(locks.Lock(&first, database, LockMode::kIx, nullptr));
  ASSERT_TRUE(locks.Lock(&first, key, LockMode::kX, nullptr));
  std::atomic<bool> got{false};
  std::thread waiter([&] {
    Transaction blocked(2, IsolationLevel::kReadCommitted);
    locks.Lock(&blocked, database, LockMode::kIx, nullptr);
    locks.Lock(&blocked, key, LockMode::kX, nullptr);
    got.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  EXPECT_FALSE(got.load());
  EXPECT_TRUE(locks.Unlock(&first, key));
  waiter.join();
  EXPECT_TRUE(got.load());

  Transaction reader(3, IsolationLevel::kReadCommitted);
  Transaction other(4, IsolationLevel::kReadCommitted);
  LockResource shared = KeyResource(TripleKey::Encode(1, 1, 1));
  ASSERT_TRUE(locks.Lock(&reader, shared, LockMode::kS, nullptr));
  ASSERT_TRUE(locks.Lock(&other, shared, LockMode::kS, nullptr));
  EXPECT_TRUE(locks.Unlock(&reader, shared));
  EXPECT_TRUE(locks.Unlock(&other, shared));
  ASSERT_TRUE(locks.Lock(&reader, shared, LockMode::kS, nullptr));
  ASSERT_TRUE(locks.Lock(&reader, shared, LockMode::kX, nullptr));
  const auto held = locks.GetLocks(reader.GetId());
  EXPECT_FALSE(held.empty());
}

}  // namespace
}  // namespace ontodb
