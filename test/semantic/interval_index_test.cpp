#include <algorithm>
#include <map>
#include <random>
#include <set>

#include <gtest/gtest.h>

#include "dictionary/mem_dictionary.h"
#include "semantic/label_index.h"
#include "semantic/type_closure.h"
#include "storage/env.h"
#include "store/mem_store.h"

namespace ontodb {
namespace {

using Edges = std::vector<std::pair<term_id_t, term_id_t>>;

TEST(P14_2_LabelIndex, ScanReturnsMembersOfTheIntervals) {
  test::DiskEnv env(16);
  LabelIndex index(&env.bpm);
  for (uint32_t label = 1; label <= 40; label++) {
    index.Insert(label, 1000 + label);
    index.Insert(label, 2000 + label);
  }
  EXPECT_FALSE(index.Insert(5, 1005));
  LabelIndex::ScanStats stats;
  const auto got = index.Scan({{3, 4}, {10, 10}, {40, 40}}, &stats);
  EXPECT_EQ(stats.scans, 3u);
  EXPECT_EQ(stats.keys, 8u);
  EXPECT_EQ(got, (std::vector<term_id_t>{1003, 1004, 1010, 1040, 2003, 2004, 2010, 2040}));
  EXPECT_TRUE(index.Scan({{41, 50}}).empty());
  EXPECT_TRUE(index.Remove(10, 1010));
  EXPECT_EQ(index.Scan({{10, 10}}), (std::vector<term_id_t>{2010}));
  EXPECT_EQ(index.Tree()->CheckInvariants(), std::nullopt);
}

TEST(P14_2_LabelIndex, BulkLoadMatchesInsertAndCrossesLeaves) {
  test::DiskEnv env(64);
  LabelIndex bulk(&env.bpm), single(&env.bpm);
  std::vector<std::pair<uint32_t, term_id_t>> pairs;
  std::mt19937_64 rng(3);
  for (int i = 0; i < 3000; i++) {
    pairs.emplace_back(1 + rng() % 500, 1 + rng() % 5000);
  }
  bulk.BulkLoad(pairs);
  for (const auto& [l, m] : pairs) {
    single.Insert(l, m);
  }
  const std::vector<Interval> ranges{{1, 500}, {100, 130}, {499, 500}};
  for (const auto& iv : ranges) {
    EXPECT_EQ(bulk.Scan({iv}), single.Scan({iv}));
  }
  EXPECT_EQ(bulk.Tree()->CheckInvariants(), std::nullopt);
}

auto DrainAll(TripleStore* store) -> std::vector<Triple> {
  auto scan = store->Scan(TriplePattern{});
  scan->Init();
  std::vector<Triple> out;
  Triple t;
  while (scan->Next(&t)) {
    out.push_back(t);
  }
  return out;
}

// A small ontology: 10 Thing; 11, 12 under 10; 13, 14 under 11; 14 also under 12.
// Instances 100.. carry one or two types.
struct World {
  MemDictionary dict;
  MemStore store;
  term_id_t type_, sub_;
  Edges edges;

  World() {
    type_ = dict.Insert("<type>");
    sub_ = dict.Insert("<sub>");
  }
};

TEST(P14_2_TypeClosure, ThreeStrategiesAgree) {
  test::DiskEnv env(64);
  World w;
  w.edges = {{11, 10}, {12, 10}, {13, 11}, {14, 11}, {14, 12}};
  for (const auto& [c, p] : w.edges) {
    w.store.Insert(Triple{c, w.sub_, p});
  }
  const std::vector<std::pair<term_id_t, term_id_t>> typed{{100, 13}, {101, 14}, {102, 12}, {103, 10}, {104, 13}, {104, 14}, {105, 99}};
  for (const auto& [x, c] : typed) {
    w.store.Insert(Triple{x, w.type_, c});
  }
  // Class 99 is only ever an rdf:type object, so it is not in any edge.
  const auto edges = CollectSubClassEdges(&w.store, w.sub_);
  EXPECT_EQ(edges.size(), 5u);
  const auto isolated = CollectTypeClasses(&w.store, w.type_);
  const auto h = ClassHierarchy::Build(edges, ClassHierarchy::TreeParent::kDeepest, isolated);
  EXPECT_TRUE(h.Contains(99));
  LabelIndex index(&env.bpm);
  index.BulkLoad(TypeLabelPairs(h, &w.store, w.type_));

  MemStore materialized;
  for (const auto& [x, c] : typed) {
    materialized.Insert(Triple{x, w.type_, c});
  }
  const size_t added = MaterializeTypes(h, &materialized, w.type_);
  EXPECT_GT(added, 0u);

  for (const term_id_t c : {10, 11, 12, 13, 14, 99}) {
    const auto by_interval = InstancesByIntervals(h, &index, c);
    const auto by_rewrite = InstancesByRewrite(h, &w.store, w.type_, c);
    const auto by_scan = InstancesByScan(&materialized, w.type_, c);
    EXPECT_EQ(by_interval, by_rewrite) << c;
    EXPECT_EQ(by_interval, by_scan) << c;
  }
  EXPECT_EQ(InstancesByIntervals(h, &index, 11), (std::vector<term_id_t>{100, 101, 104}));
  EXPECT_EQ(InstancesByIntervals(h, &index, 10), (std::vector<term_id_t>{100, 101, 102, 103, 104}));
}

TEST(P14_2_TypeClosure, RandomOntologiesAgree) {
  std::mt19937_64 rng(11);
  for (int round = 0; round < 25; round++) {
    test::DiskEnv env(64);
    World w;
    const int n = 4 + static_cast<int>(rng() % 30);
    for (int i = 1; i < n; i++) {
      const int parents = 1 + static_cast<int>(rng() % 2);
      for (int k = 0; k < parents; k++) {
        w.store.Insert(Triple{static_cast<term_id_t>(100 + i), w.sub_, static_cast<term_id_t>(100 + rng() % i)});
      }
    }
    const int instances = 50 + static_cast<int>(rng() % 100);
    for (int x = 0; x < instances; x++) {
      const int types = 1 + static_cast<int>(rng() % 3);
      for (int k = 0; k < types; k++) {
        w.store.Insert(Triple{static_cast<term_id_t>(5000 + x), w.type_, static_cast<term_id_t>(100 + rng() % n)});
      }
    }
    const auto h = ClassHierarchy::Build(CollectSubClassEdges(&w.store, w.sub_), ClassHierarchy::TreeParent::kDeepest,
                                         CollectTypeClasses(&w.store, w.type_));
    LabelIndex index(&env.bpm);
    index.BulkLoad(TypeLabelPairs(h, &w.store, w.type_));
    MemStore closed;
    for (const Triple& t : DrainAll(&w.store)) {
      if (t.predicate == w.type_) {
        closed.Insert(t);
      }
    }
    MaterializeTypes(h, &closed, w.type_);
    for (int c = 0; c < n; c++) {
      const term_id_t cls = static_cast<term_id_t>(100 + c);
      const auto want = InstancesByRewrite(h, &w.store, w.type_, cls);
      ASSERT_EQ(InstancesByIntervals(h, &index, cls), want) << round << " " << cls;
      ASSERT_EQ(InstancesByScan(&closed, w.type_, cls), want) << round << " " << cls;
    }
  }
}

}  // namespace
}  // namespace ontodb
