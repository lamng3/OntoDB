#include <gtest/gtest.h>

#include <map>
#include <random>
#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "common/exception.h"
#include "dictionary/disk_dictionary.h"
#include "loader/rdf_loader.h"
#include "storage/env.h"
#include "storage/index/b_plus_tree.h"
#include "storage/page/b_plus_tree_header_page.h"
#include "storage/page/term_heap_page.h"
#include "storage/term/term_heap.h"
#include "store/indexed_store.h"

namespace ontodb {
namespace {

auto Collect(TripleStore* store, const TriplePattern& pattern) -> std::vector<Triple> {
  auto iter = store->Scan(pattern);
  iter->Init();
  std::vector<Triple> triples;
  Triple triple;
  while (iter->Next(&triple)) {
    triples.push_back(triple);
  }
  return triples;
}

TEST(P3_1_TermHeap, SlottedPageRoundTrip) {
  Page raw;
  TermHeapPage page(&raw);
  page.Init();
  const auto before = page.FreeSpace();
  EXPECT_GT(before, 0);
  const auto slot = page.Insert("alice");
  ASSERT_TRUE(slot.has_value());
  EXPECT_EQ(page.Get(*slot), "alice");
  EXPECT_LT(page.FreeSpace(), before);
  EXPECT_GE(page.GetSlotCount(), 1);
  const auto second = page.Insert("");
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(page.Get(*second), "");
  const auto used = page.FreeSpace();
  page.Delete(*slot);
  EXPECT_GT(page.FreeSpace(), used);
  EXPECT_FALSE(page.Insert(std::string(PAGE_SIZE, 'x')).has_value());
}

TEST(P3_1_TermHeap, HeapSurvivesChainAndOversized) {
  test::DiskEnv env;
  page_id_t first = INVALID_PAGE_ID;
  ASSERT_NE(env.bpm.NewPage(&first), nullptr);
  EXPECT_TRUE(env.bpm.UnpinPage(first, true));
  TermHeap heap(&env.bpm, first);
  EXPECT_THROW(heap.Insert(std::string(PAGE_SIZE, 'z')), StorageException);
  const auto a = heap.Insert("alpha");
  const auto b = heap.Insert(std::string(3000, 'b'));
  const auto c = heap.Insert("gamma");
  EXPECT_EQ(heap.Get(a), "alpha");
  EXPECT_EQ(heap.Get(b), std::string(3000, 'b'));
  EXPECT_EQ(heap.Get(c), "gamma");
  EXPECT_TRUE(heap.Delete(b));
  EXPECT_FALSE(heap.Delete(b));
  int live = 0;
  for (auto it = heap.Begin(); !it.IsEnd(); ++it) {
    live++;
  }
  EXPECT_EQ(live, 2);
  EXPECT_EQ(env.bpm.GetPinnedFrameCount(), 0u);
}

// Delete compacts the page, so records move; their slot numbers must not.
TEST(P3_1_TermHeap, CompactionKeepsSlotsAndContentsAgainstAMap) {
  Page raw;
  TermHeapPage page(&raw);
  page.Init();
  std::map<slot_offset_t, std::string> model;
  std::mt19937 rng(7);
  for (int step = 0; step < 4000; step++) {
    if (model.empty() || rng() % 3 != 0) {
      const std::string bytes(rng() % 90, static_cast<char>('a' + rng() % 26));
      const auto before = page.FreeSpace();
      if (const auto slot = page.Insert(bytes)) {
        ASSERT_FALSE(model.count(*slot)) << "slot " << *slot << " was live";
        model[*slot] = bytes;
      } else {
        ASSERT_LT(before, bytes.size() + 4);  // only refuses when it cannot fit
      }
    } else {
      auto it = model.begin();
      std::advance(it, static_cast<long>(rng() % model.size()));
      page.Delete(it->first);
      model.erase(it);
    }
    for (const auto& [slot, bytes] : model) {
      ASSERT_EQ(page.Get(slot), bytes) << "step " << step;
    }
  }
  for (auto it = model.begin(); it != model.end(); it = model.erase(it)) {
    page.Delete(it->first);
  }
  EXPECT_EQ(page.GetSlotCount(), 0);
  EXPECT_EQ(page.FreeSpace(), PAGE_SIZE - 16);
}

TEST(P3_2_DiskDictionary, DISABLED_BothDirectionsAndRestart) {
  test::TempDir dir;
  const auto path = (dir.path() / "db").string();
  term_id_t id = INVALID_TERM_ID;
  {
    DiskManager disk(path);
    BufferPoolManager bpm(8, &disk, 2);
    Catalog catalog(&bpm);
    DiskDictionary dict(&bpm, &catalog);
    id = dict.Insert("<http://ex/Alice>");
    EXPECT_EQ(dict.Insert("<http://ex/Alice>"), id);
    EXPECT_EQ(dict.Lookup("<http://ex/Alice>"), id);
    EXPECT_EQ(dict.Lookup(id), "<http://ex/Alice>");
    EXPECT_EQ(dict.Size(), 1u);
    EXPECT_FALSE(dict.Lookup("<missing>").has_value());
    bpm.FlushAllPages();
    catalog.Flush();
    disk.ShutDown();
  }
  DiskManager disk(path);
  BufferPoolManager bpm(8, &disk, 2);
  Catalog catalog(&bpm);
  DiskDictionary dict(&bpm, &catalog);
  EXPECT_EQ(dict.Lookup("<http://ex/Alice>"), id);
  EXPECT_EQ(dict.Lookup(id), "<http://ex/Alice>");
  EXPECT_EQ(bpm.GetPinnedFrameCount(), 0u);
}

TEST(P3_3_Catalog, DISABLED_RootsSurviveFlush) {
  test::DiskEnv env;
  Catalog catalog(&env.bpm, 0);
  EXPECT_EQ(catalog.GetIndexRoot("spo"), INVALID_PAGE_ID);
  catalog.SetIndexRoot("spo", 4);
  catalog.SetIndexRoot("pos", 5);
  catalog.SetDictionaryRoot(6);
  EXPECT_EQ(catalog.GetIndexRoot("spo"), 4);
  EXPECT_EQ(catalog.GetIndexRoot("nope"), INVALID_PAGE_ID);
  catalog.Flush();
  env.bpm.FlushAllPages();
  Catalog again(&env.bpm, 0);
  EXPECT_EQ(again.GetIndexRoot("spo"), 4);
  EXPECT_EQ(again.GetIndexRoot("pos"), 5);
  EXPECT_EQ(again.GetDictionaryRoot(), 6);
  EXPECT_EQ(env.bpm.GetPinnedFrameCount(), 0u);
}

TEST(P3_4_IndexedStore, DISABLED_InsertDeleteScan) {
  test::DiskEnv env;
  Catalog catalog(&env.bpm);
  DiskDictionary dict(&env.bpm, &catalog);
  IndexedStore store(&catalog, &env.bpm, &dict);
  const Triple triple{1, 2, 3};
  EXPECT_TRUE(store.Insert(triple));
  EXPECT_FALSE(store.Insert(triple));
  EXPECT_EQ(Collect(&store, {}).size(), 1u);
  EXPECT_TRUE(store.Delete(triple));
  EXPECT_FALSE(store.Delete(triple));
  EXPECT_TRUE(Collect(&store, {}).empty());
  EXPECT_EQ(env.bpm.GetPinnedFrameCount(), 0u);
}

TEST(P3_5_BoundPatterns, DISABLED_AllEightCombinations) {
  test::DiskEnv env;
  Catalog catalog(&env.bpm);
  DiskDictionary dict(&env.bpm, &catalog);
  IndexedStore store(&catalog, &env.bpm, &dict);
  const std::vector<Triple> triples = {{1, 10, 100}, {1, 10, 101}, {2, 11, 100}};
  for (const auto& triple : triples) {
    EXPECT_TRUE(store.Insert(triple));
  }
  auto count = [&](TriplePattern pattern) { return Collect(&store, pattern).size(); };
  EXPECT_EQ(count({}), 3u);
  EXPECT_EQ(count({1, std::nullopt, std::nullopt}), 2u);
  EXPECT_EQ(count({std::nullopt, 10, std::nullopt}), 2u);
  EXPECT_EQ(count({std::nullopt, std::nullopt, 100}), 2u);
  EXPECT_EQ(count({1, 10, std::nullopt}), 2u);
  EXPECT_EQ(count({1, std::nullopt, 100}), 1u);
  EXPECT_EQ(count({std::nullopt, 10, 100}), 1u);
  EXPECT_EQ(count({1, 10, 100}), 1u);
  EXPECT_EQ(count({9, 9, 9}), 0u);
  EXPECT_EQ(env.bpm.GetPinnedFrameCount(), 0u);
}

TEST(P3_6_Restart, DISABLED_CleanShutdownKeepsTriples) {
  test::TempDir dir;
  const auto path = (dir.path() / "db").string();
  {
    DiskManager disk(path);
    BufferPoolManager bpm(8, &disk, 2);
    Catalog catalog(&bpm);
    DiskDictionary dict(&bpm, &catalog);
    IndexedStore store(&catalog, &bpm, &dict);
    EXPECT_TRUE(store.Insert({7, 8, 9}));
    bpm.FlushAllPages();
    catalog.Flush();
    disk.ShutDown();
  }
  DiskManager disk(path);
  BufferPoolManager bpm(8, &disk, 2);
  Catalog catalog(&bpm);
  DiskDictionary dict(&bpm, &catalog);
  IndexedStore store(&catalog, &bpm, &dict);
  const auto got = Collect(&store, {});
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0], (Triple{7, 8, 9}));
  EXPECT_EQ(bpm.GetPinnedFrameCount(), 0u);
}

TEST(P6_1_BulkLoader, DISABLED_LoadTinyThroughIndexes) {
  test::DiskEnv env(16, 2);
  Catalog catalog(&env.bpm);
  DiskDictionary dict(&env.bpm, &catalog);
  IndexedStore store(&catalog, &env.bpm, &dict);
  const auto loaded = RdfLoader().Load(test::DataFile("tiny.ttl"), dict, store);
  EXPECT_EQ(loaded.triples, 53u);
  EXPECT_EQ(Collect(&store, {}).size(), 53u);
  const auto root = catalog.GetIndexRoot("spo");
  EXPECT_NE(root, INVALID_PAGE_ID);
  BPlusTree tree("spo", root, &env.bpm);
  EXPECT_FALSE(tree.CheckInvariants().has_value());
  EXPECT_EQ(env.bpm.GetPinnedFrameCount(), 0u);
}

}  // namespace
}  // namespace ontodb
