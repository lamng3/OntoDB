#include <gtest/gtest.h>

#include <atomic>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include "storage/env.h"
#include "storage/index/b_plus_tree.h"
#include "storage/page/b_plus_tree_header_page.h"

namespace ontodb {
namespace {

// A pool with several shards pins resident pages without the shard latch. These tests hammer it
// from several threads, with more pages than frames so that eviction runs against the readers.

auto Header(BufferPoolManager* bpm) -> page_id_t {
  page_id_t id = INVALID_PAGE_ID;
  Page* page = bpm->NewPage(&id);
  EXPECT_NE(page, nullptr);
  BPlusTreeHeaderPage header(page);
  header.Init();
  header.SetRootPageId(INVALID_PAGE_ID);
  EXPECT_TRUE(bpm->UnpinPage(id, true));
  return id;
}

TEST(ShardedPool, EvictionNeverReturnsAnotherPagesBytes) {
  test::DiskEnv env(64, 2, nullptr, 4);
  const int pages = 400;
  std::vector<page_id_t> ids;
  for (int i = 0; i < pages; i++) {
    page_id_t id = INVALID_PAGE_ID;
    {
      WritePageGuard guard = env.bpm.NewPageGuarded(&id);
      char* data = guard.GetData();
      const std::string stamp = "page-" + std::to_string(id);
      std::copy(stamp.begin(), stamp.end(), data + 16);
    }
    ids.push_back(id);
  }
  std::atomic<int> wrong{0};
  std::atomic<uint64_t> reads{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; t++) {
    threads.emplace_back([&, t] {
      std::mt19937 rng(100 + t);
      for (int i = 0; i < 20000; i++) {
        // Half the reads go to a few hot pages, half to any page, so hits and evictions mix.
        const page_id_t id = (rng() % 2 == 0) ? ids[rng() % 8] : ids[rng() % ids.size()];
        ReadPageGuard guard = env.bpm.FetchPageRead(id);
        const std::string expect = "page-" + std::to_string(id);
        if (std::string(guard.GetData() + 16, expect.size()) != expect) {
          wrong++;
        }
        reads++;
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(wrong.load(), 0);
  EXPECT_EQ(reads.load(), 8u * 20000u);
  EXPECT_EQ(env.bpm.GetPinnedFrameCount(), 0u);
}

TEST(ShardedPool, ConcurrentInsertRemoveAndScanKeepTheTreeSound) {
  test::DiskEnv env(2048, 2, nullptr, 8);
  BPlusTree tree("spo", Header(&env.bpm), &env.bpm, nullptr, 4, 4);  // tiny nodes: many splits
  const int per_thread = 1500;
  // Keys 1..3000 are present before the run; writers add 3000+ and remove the first half.
  for (term_id_t i = 1; i <= 3000; i++) {
    ASSERT_TRUE(tree.Insert(TripleKey::Encode(i, 1, 1)));
  }
  std::atomic<bool> stop{false};
  std::atomic<int> unsorted{0};
  std::vector<std::thread> readers;
  for (int r = 0; r < 2; r++) {
    readers.emplace_back([&] {
      while (!stop.load()) {
        TripleKey previous = TripleKey::Encode(0, 0, 0);
        bool first = true;
        for (auto it = tree.BeginRange(TripleKey::Encode(2900, 0, 0), TripleKey::Encode(3400, 0, 0)); !it.IsEnd();
             ++it) {
          if (!first && (*it).Compare(previous) <= 0) {
            unsorted++;
          }
          previous = *it;
          first = false;
        }
      }
    });
  }
  std::vector<std::thread> writers;
  for (int w = 0; w < 4; w++) {
    writers.emplace_back([&, w] {
      for (int i = 0; i < per_thread; i++) {
        EXPECT_TRUE(tree.Insert(TripleKey::Encode(static_cast<term_id_t>(3001 + w * per_thread + i), 1, 1)));
      }
    });
  }
  for (int w = 0; w < 2; w++) {
    writers.emplace_back([&, w] {
      for (term_id_t i = 1 + static_cast<term_id_t>(w); i <= 1500; i += 2) {
        EXPECT_TRUE(tree.Remove(TripleKey::Encode(i, 1, 1)));
      }
    });
  }
  for (auto& thread : writers) {
    thread.join();
  }
  stop = true;
  for (auto& thread : readers) {
    thread.join();
  }
  EXPECT_EQ(unsorted.load(), 0);
  const auto broken = tree.CheckInvariants();
  ASSERT_FALSE(broken.has_value()) << *broken;
  std::set<term_id_t> expect;
  for (term_id_t i = 1501; i <= 3000; i++) {
    expect.insert(i);
  }
  for (term_id_t i = 3001; i <= 3000 + 4 * per_thread; i++) {
    expect.insert(i);
  }
  std::set<term_id_t> got;
  for (auto it = tree.Begin(); !it.IsEnd(); ++it) {
    term_id_t s = 0;
    term_id_t p = 0;
    term_id_t o = 0;
    (*it).Decode(&s, &p, &o);
    got.insert(s);
  }
  EXPECT_EQ(got, expect);
  EXPECT_EQ(env.bpm.GetPinnedFrameCount(), 0u);
}

}  // namespace
}  // namespace ontodb
