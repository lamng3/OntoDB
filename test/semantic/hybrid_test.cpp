#include <algorithm>
#include <random>
#include <set>

#include <gtest/gtest.h>

#include "dictionary/mem_dictionary.h"
#include "semantic/hybrid_search.h"
#include "storage/env.h"

namespace ontodb {
namespace {

// A three-level tree: one root, `fan` children, `fan` grandchildren under
// each child. A class's vector is its parent's plus noise, so subtrees cluster.
struct Ontology {
  MemDictionary dict;
  VectorStore vectors{24};
  std::vector<std::pair<term_id_t, term_id_t>> edges;
  term_id_t root{0};
  std::vector<term_id_t> children;

  explicit Ontology(uint32_t fan, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<float> gauss;
    auto noisy = [&](const std::vector<float>& base, float spread) {
      std::vector<float> v(base);
      for (float& x : v) {
        x += spread * gauss(rng);
      }
      return v;
    };
    std::vector<float> top(24);
    for (float& x : top) {
      x = gauss(rng);
    }
    root = dict.Insert("<root>");
    vectors.Put(root, top.data());
    for (uint32_t c = 0; c < fan; c++) {
      const term_id_t child = dict.Insert("<c" + std::to_string(c) + ">");
      const auto cv = noisy(top, 0.8f);
      vectors.Put(child, cv.data());
      edges.emplace_back(child, root);
      children.push_back(child);
      for (uint32_t g = 0; g < fan; g++) {
        const term_id_t leaf = dict.Insert("<c" + std::to_string(c) + "g" + std::to_string(g) + ">");
        const auto gv = noisy(cv, 0.3f);
        vectors.Put(leaf, gv.data());
        edges.emplace_back(leaf, child);
      }
    }
  }
};

auto Ids(const std::vector<Neighbor>& v) -> std::vector<term_id_t> {
  std::vector<term_id_t> out;
  for (const auto& n : v) {
    out.push_back(n.id);
  }
  return out;
}

TEST(P14_7_HybridSearch, EveryStrategyReturnsOnlyMembersAndPreFilterIsExact) {
  test::DiskEnv env(256);
  Ontology o(12, 1);
  const auto h = ClassHierarchy::Build(o.edges);
  LabelIndex classes(&env.bpm);
  std::vector<std::pair<uint32_t, term_id_t>> pairs;
  for (const auto& [c, p] : o.edges) {
    pairs.emplace_back(h.Label(c), c);
    pairs.emplace_back(h.Label(p), p);
  }
  classes.BulkLoad(pairs);
  FlatIndex flat(&o.vectors, FlatIndex::Precision::kFloat32);
  IvfIndex ivf(&o.vectors, &env.bpm, {.lists = 8});
  ivf.Build();
  HnswIndex hnsw(&o.vectors, {.m = 12, .ef_construction = 80});
  hnsw.Build();
  HybridSearch search({&h, &classes, &o.vectors, &flat, &ivf, &hnsw});

  const term_id_t under = o.children[3];
  const auto below = h.Descendants(under);
  const std::set<term_id_t> members(below.begin(), below.end());
  ASSERT_EQ(members.size(), 13u);
  // The query is a vector from another subtree, so the filter matters.
  const float* q = o.vectors.Vector(*o.vectors.Row(o.children[7]));

  // Ground truth: score the members exactly.
  std::vector<uint32_t> rows;
  for (const term_id_t id : members) {
    rows.push_back(*o.vectors.Row(id));
  }
  const auto truth = flat.SearchRows(q, 5, rows);

  HybridSearch::Params params{.nprobe = 8, .ef = 64};
  const auto pre = search.Similar(q, 5, under, HybridSearch::Strategy::kPreFilter, params);
  EXPECT_EQ(Ids(pre.neighbors), Ids(truth));
  EXPECT_EQ(pre.members, 13u);
  EXPECT_GE(pre.scans.scans, 1u);
  EXPECT_EQ(pre.stats.scored, 13u);

  for (const auto strategy : {HybridSearch::Strategy::kPostFilterHnsw, HybridSearch::Strategy::kPostFilterIvf,
                              HybridSearch::Strategy::kMaskHnsw, HybridSearch::Strategy::kMaskIvf,
                              HybridSearch::Strategy::kAuto}) {
    const auto got = search.Similar(q, 5, under, strategy, params);
    EXPECT_EQ(got.neighbors.size(), 5u) << HybridSearch::Name(strategy);
    for (const auto& n : got.neighbors) {
      EXPECT_EQ(members.count(n.id), 1u) << HybridSearch::Name(strategy);
    }
    // With full probing the mask and post-filter answers are the exact ones.
    if (strategy == HybridSearch::Strategy::kMaskIvf || strategy == HybridSearch::Strategy::kPostFilterIvf) {
      const auto full = search.Similar(q, 5, under, strategy, {.nprobe = 8});
      EXPECT_EQ(Ids(full.neighbors), Ids(truth)) << HybridSearch::Name(strategy);
    }
  }
}

TEST(P14_7_HybridSearch, AutoChoosesBySubtreeSizeAndEdgeCasesAreEmpty) {
  test::DiskEnv env(256);
  Ontology o(12, 2);
  const auto h = ClassHierarchy::Build(o.edges);
  LabelIndex classes(&env.bpm);
  std::vector<std::pair<uint32_t, term_id_t>> pairs;
  for (const auto& [c, p] : o.edges) {
    pairs.emplace_back(h.Label(c), c);
    pairs.emplace_back(h.Label(p), p);
  }
  classes.BulkLoad(pairs);
  FlatIndex flat(&o.vectors, FlatIndex::Precision::kFloat32);
  HnswIndex hnsw(&o.vectors, {.m = 12, .ef_construction = 80});
  hnsw.Build();
  HybridSearch search({&h, &classes, &o.vectors, &flat, nullptr, &hnsw});
  const float* q = o.vectors.Vector(0);
  HybridSearch::Params params{.ef = 64, .pre_threshold = 20};

  EXPECT_EQ(search.Similar(q, 3, o.children[0], HybridSearch::Strategy::kAuto, params).used, HybridSearch::Strategy::kPreFilter);
  EXPECT_EQ(search.Similar(q, 3, o.root, HybridSearch::Strategy::kAuto, params).used, HybridSearch::Strategy::kMaskHnsw);
  EXPECT_EQ(search.Similar(q, 3, INVALID_TERM_ID, HybridSearch::Strategy::kAuto, params).neighbors.size(), 3u);

  // A leaf has only itself below it: fewer than k results, not an error.
  const term_id_t leaf = *o.dict.Lookup("<c0g0>");
  const auto one = search.Similar(q, 5, leaf, HybridSearch::Strategy::kPreFilter, params);
  ASSERT_EQ(one.neighbors.size(), 1u);
  EXPECT_EQ(one.neighbors[0].id, leaf);
  // A class the hierarchy does not know has no members.
  EXPECT_TRUE(search.Similar(q, 5, 424242, HybridSearch::Strategy::kAuto, params).neighbors.empty());
}

}  // namespace
}  // namespace ontodb
