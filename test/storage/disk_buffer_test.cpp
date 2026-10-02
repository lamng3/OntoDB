#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstring>
#include <future>
#include <sstream>
#include <thread>
#include <vector>

#include "buffer/lru_k_replacer.h"
#include "common/database.h"
#include "common/exception.h"
#include "common/trace.h"
#include "shell/shell.h"
#include "storage/disk/disk_scheduler.h"
#include "storage/env.h"

namespace ontodb {
namespace {

auto Pattern(char mark) -> std::array<char, PAGE_SIZE> {
  std::array<char, PAGE_SIZE> page{};
  for (size_t i = 0; i < PAGE_SIZE; i++) {
    page[i] = static_cast<char>(static_cast<unsigned char>(i) ^ static_cast<unsigned char>(mark));
  }
  return page;
}

TEST(P1_1_DiskManager, DISABLED_ReadWriteAndRestart) {
  test::TempDir dir;
  const auto path = (dir.path() / "db").string();
  IoStats stats;
  page_id_t written = INVALID_PAGE_ID;
  page_id_t untouched = INVALID_PAGE_ID;
  const auto image = Pattern(0x5A);
  {
    DiskManager disk(path, &stats);
    written = disk.AllocatePage();
    untouched = disk.AllocatePage();
    EXPECT_NE(written, untouched);
    disk.WritePage(written, image.data());
    std::array<char, PAGE_SIZE> back{};
    disk.ReadPage(written, back.data());
    EXPECT_EQ(back, image);
    std::array<char, PAGE_SIZE> zeros{};
    disk.ReadPage(untouched, zeros.data());
    EXPECT_EQ(zeros, (std::array<char, PAGE_SIZE>{}));
    EXPECT_GE(disk.GetNumWrites(), 1);
    EXPECT_GE(disk.GetNumReads(), 1);
    EXPECT_GE(stats.page_writes(), 1u);
    EXPECT_GE(stats.page_reads(), 1u);
    disk.DeallocatePage(untouched);
    disk.ShutDown();
  }
  DiskManager again(path, &stats);
  std::array<char, PAGE_SIZE> back{};
  again.ReadPage(written, back.data());
  EXPECT_EQ(back, image);
}

TEST(P1_2_DiskScheduler, DISABLED_BackgroundReadWrite) {
  test::TempDir dir;
  DiskManager disk((dir.path() / "db").string());
  DiskScheduler scheduler(&disk);
  const auto image = Pattern(3);
  auto write_buf = image;
  DiskRequest write;
  write.is_write = true;
  write.page_id = 1;
  write.data = write_buf.data();
  auto write_done = write.callback.get_future();
  scheduler.Schedule(std::move(write));
  EXPECT_TRUE(write_done.get());

  std::array<char, PAGE_SIZE> read_buf{};
  DiskRequest read;
  read.is_write = false;
  read.page_id = 1;
  read.data = read_buf.data();
  auto read_done = read.callback.get_future();
  scheduler.Schedule(std::move(read));
  EXPECT_TRUE(read_done.get());
  EXPECT_EQ(read_buf, image);

  constexpr int kN = 8;
  std::vector<std::array<char, PAGE_SIZE>> buffers(kN);
  std::vector<std::future<bool>> futures;
  for (int i = 0; i < kN; i++) {
    buffers[i] = Pattern(static_cast<char>(10 + i));
    DiskRequest req;
    req.is_write = true;
    req.page_id = 10 + i;
    req.data = buffers[i].data();
    futures.push_back(req.callback.get_future());
    scheduler.Schedule(std::move(req));
  }
  for (auto& fut : futures) {
    EXPECT_TRUE(fut.get());
  }
}

TEST(P1_3_LRUKReplacer, DISABLED_EvictsByKDistance) {
  LRUKReplacer replacer(3, 2);
  frame_id_t none = 77;
  EXPECT_FALSE(replacer.Evict(&none));
  EXPECT_EQ(none, 77);
  replacer.RecordAccess(0);
  replacer.SetEvictable(0, true);
  frame_id_t victim = -1;
  ASSERT_TRUE(replacer.Evict(&victim));
  EXPECT_EQ(victim, 0);
  EXPECT_EQ(replacer.Size(), 0u);

  replacer.RecordAccess(1);
  replacer.RecordAccess(1);
  replacer.RecordAccess(2);
  replacer.SetEvictable(1, true);
  replacer.SetEvictable(2, true);
  ASSERT_TRUE(replacer.Evict(&victim));
  EXPECT_EQ(victim, 2);

  LRUKReplacer ranked(3, 2);
  for (frame_id_t frame : {0, 1, 2, 0, 1, 2}) {
    ranked.RecordAccess(frame);
  }
  for (frame_id_t frame = 0; frame < 3; frame++) {
    ranked.SetEvictable(frame, true);
  }
  EXPECT_EQ(ranked.Size(), 3u);
  ASSERT_TRUE(ranked.Evict(&victim));
  EXPECT_EQ(victim, 0);
  ASSERT_TRUE(ranked.Evict(&victim));
  EXPECT_EQ(victim, 1);
  ranked.SetEvictable(2, false);
  EXPECT_EQ(ranked.Size(), 0u);
  EXPECT_FALSE(ranked.Evict(&victim));
  ranked.Remove(2);
  EXPECT_EQ(ranked.Size(), 0u);
}

TEST(P1_3_LRUKReplacer, DISABLED_ConcurrentAccess) {
  LRUKReplacer replacer(16, 2);
  EXPECT_EQ(replacer.Size(), 0u);
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; t++) {
    threads.emplace_back([&replacer, t] {
      for (int i = 0; i < 100; i++) {
        replacer.RecordAccess(static_cast<frame_id_t>((t + i) % 16));
        replacer.SetEvictable(static_cast<frame_id_t>((t + i) % 16), true);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  frame_id_t victim = -1;
  while (replacer.Evict(&victim)) {
  }
  EXPECT_EQ(replacer.Size(), 0u);
}

TEST(P1_4_BufferPoolManager, DISABLED_EvictsAndPins) {
  test::DiskEnv env(3, 2);
  EXPECT_EQ(env.bpm.GetPinnedFrameCount(), 0u);
  std::vector<page_id_t> ids;
  for (int i = 0; i < 3; i++) {
    page_id_t id = INVALID_PAGE_ID;
    Page* page = env.bpm.NewPage(&id);
    ASSERT_NE(page, nullptr);
    page->GetData()[100] = static_cast<char>(i + 1);
    ids.push_back(id);
    EXPECT_TRUE(env.bpm.UnpinPage(id, true));
  }
  EXPECT_EQ(env.bpm.GetPinnedFrameCount(), 0u);
  page_id_t extra = INVALID_PAGE_ID;
  Page* evicted = env.bpm.NewPage(&extra);
  ASSERT_NE(evicted, nullptr);
  EXPECT_TRUE(env.bpm.UnpinPage(extra, false));
  Page* again = env.bpm.FetchPage(ids[0]);
  ASSERT_NE(again, nullptr);
  EXPECT_EQ(again->GetData()[100], 1);
  EXPECT_TRUE(env.bpm.UnpinPage(ids[0], false));
  EXPECT_GE(env.stats.buffer_misses(), 1u);
  Page* hit = env.bpm.FetchPage(ids[0]);
  ASSERT_NE(hit, nullptr);
  EXPECT_TRUE(env.bpm.UnpinPage(ids[0], false));
  EXPECT_GE(env.stats.buffer_hits(), 1u);
  EXPECT_TRUE(env.bpm.DeletePage(extra));
  EXPECT_EQ(env.bpm.FetchPage(extra), nullptr);
  EXPECT_EQ(env.bpm.GetPinnedFrameCount(), 0u);
}

TEST(P1_4_BufferPoolManager, DISABLED_ConcurrentFetch) {
  test::DiskEnv env(8, 2);
  page_id_t id = INVALID_PAGE_ID;
  Page* page = env.bpm.NewPage(&id);
  ASSERT_NE(page, nullptr);
  std::memset(page->GetData(), 7, PAGE_SIZE);
  EXPECT_TRUE(env.bpm.UnpinPage(id, true));
  std::atomic<int> ok{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; t++) {
    threads.emplace_back([&env, id, &ok] {
      for (int i = 0; i < 50; i++) {
        Page* fetched = env.bpm.FetchPage(id);
        if (fetched != nullptr && fetched->GetData()[0] == 7 && env.bpm.UnpinPage(id, false)) {
          ok.fetch_add(1);
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(ok.load(), 200);
  EXPECT_EQ(env.bpm.GetPinnedFrameCount(), 0u);
}

TEST(P1_5_PageGuard, DISABLED_MoveDropAndShare) {
  test::DiskEnv env(4, 2);
  page_id_t id = INVALID_PAGE_ID;
  {
    WritePageGuard guard = env.bpm.NewPageGuarded(&id);
    ASSERT_TRUE(guard.IsValid());
    EXPECT_EQ(guard.GetPageId(), id);
    guard.GetData()[0] = 42;
    WritePageGuard moved = std::move(guard);
    EXPECT_FALSE(guard.IsValid());
    EXPECT_EQ(moved.GetData()[0], 42);
    moved.Drop();
    EXPECT_FALSE(moved.IsValid());
  }
  EXPECT_EQ(env.bpm.GetPinnedFrameCount(), 0u);
  {
    ReadPageGuard first = env.bpm.FetchPageRead(id);
    ReadPageGuard second = env.bpm.FetchPageRead(id);
    EXPECT_EQ(first.GetData()[0], 42);
    EXPECT_EQ(second.GetData()[0], 42);
  }
  EXPECT_EQ(env.bpm.GetPinnedFrameCount(), 0u);
  {
    WritePageGuard writer = env.bpm.FetchPageWrite(id);
    writer.GetData()[1] = 9;
  }
  ReadPageGuard reader = env.bpm.FetchPageRead(id);
  EXPECT_EQ(reader.GetData()[1], 9);
  reader.Drop();
  EXPECT_EQ(env.bpm.GetPinnedFrameCount(), 0u);
}

TEST(P1_6_Inspection, DISABLED_DebugAndTrace) {
  class MemTrace : public TraceSink {
   public:
    void Event(std::string_view component, std::string_view message) override {
      events.emplace_back(component, message);
    }
    std::vector<std::pair<std::string, std::string>> events;
  } trace;
  test::DiskEnv env(4, 2, &trace);
  page_id_t id = INVALID_PAGE_ID;
  Page* page = env.bpm.NewPage(&id);
  ASSERT_NE(page, nullptr);
  EXPECT_TRUE(env.bpm.UnpinPage(id, true));
  const auto debug = env.bpm.DebugString();
  EXPECT_NE(debug.find(std::to_string(id)), std::string::npos);
  bool saw_bpm = false;
  for (const auto& event : trace.events) {
    if (event.first == "bpm") {
      saw_bpm = true;
    }
  }
  EXPECT_TRUE(saw_bpm);

  Database db;
  std::istringstream in("\\bpm\n\\trace on\n\\quit\n");
  std::ostringstream out;
  Shell shell(db, in, out);
  shell.Run();
  const auto text = out.str();
  EXPECT_EQ(text.find("not built yet"), std::string::npos) << text;
}

TEST(P8_3_Wal, DISABLED_FlushReachesCrashPointBeforeWrite) {
  test::TempDir dir;
  const auto path = (dir.path() / "db").string();
  page_id_t id = INVALID_PAGE_ID;
  {
    DiskManager disk(path);
    BufferPoolManager bpm(4, &disk, 2);
    Page* page = bpm.NewPage(&id);
    ASSERT_NE(page, nullptr);
    std::memset(page->GetData() + 8, 0x11, 16);
    EXPECT_TRUE(bpm.UnpinPage(id, true));
    test::ArmGuard arm("BufferPoolManager::FlushPage::before_disk_write");
    EXPECT_THROW(bpm.FlushPage(id), CrashInjected);
  }
  DiskManager disk(path);
  std::array<char, PAGE_SIZE> back{};
  disk.ReadPage(id, back.data());
  EXPECT_EQ(back[8], 0);
}

}  // namespace
}  // namespace ontodb
