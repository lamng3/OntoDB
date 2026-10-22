#include <algorithm>
#include <random>
#include <set>

#include <gtest/gtest.h>

#include "dictionary/mem_dictionary.h"
#include "semantic/flat_index.h"
#include "semantic/hnsw_index.h"

namespace ontodb {
namespace {

void Blobs(VectorStore* store, MemDictionary* dict, uint32_t clusters, uint32_t per, float spread, uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::normal_distribution<float> gauss;
  const uint32_t dim = store->Dim();
  std::vector<float> v(dim);
  for (uint32_t c = 0; c < clusters; c++) {
    std::vector<float> center(dim);
    for (float& x : center) {
      x = gauss(rng);
    }
    for (uint32_t i = 0; i < per; i++) {
      for (uint32_t d = 0; d < dim; d++) {
        v[d] = center[d] + spread * gauss(rng);
      }
      store->Put(dict->Insert("<t" + std::to_string(c) + "_" + std::to_string(i) + ">"), v.data());
    }
  }
}

auto Recall(const std::vector<Neighbor>& got, const std::vector<Neighbor>& truth) -> double {
  std::set<term_id_t> want;
  for (const auto& n : truth) {
    want.insert(n.id);
  }
  size_t hit = 0;
  for (const auto& n : got) {
    hit += want.count(n.id);
  }
  return truth.empty() ? 1.0 : static_cast<double>(hit) / static_cast<double>(truth.size());
}

TEST(P14_6_HnswIndex, EmptyAndSingleRow) {
  MemDictionary dict;
  VectorStore store(4);
  HnswIndex empty(&store, {});
  empty.Build();
  const float q[4] = {1, 0, 0, 0};
  EXPECT_TRUE(empty.Search(q, 3, 10).empty());
  store.Put(dict.Insert("<a>"), q);
  HnswIndex one(&store, {});
  one.Build();
  const auto got = one.Search(q, 3, 10);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_NEAR(got[0].score, 1.0f, 1e-6);
}

TEST(P14_6_HnswIndex, FindsTheTrueNeighborsOfBlobs) {
  MemDictionary dict;
  VectorStore store(32);
  Blobs(&store, &dict, 40, 50, 0.5f, 1);
  FlatIndex exact(&store, FlatIndex::Precision::kFloat32);
  HnswIndex hnsw(&store, {.m = 16, .ef_construction = 100});
  hnsw.Build();
  EXPECT_GT(hnsw.MeanDegree(), 4.0);
  double recall = 0;
  int queries = 0;
  SearchStats stats;
  for (uint32_t row = 0; row < store.Size(); row += 19) {
    recall += Recall(hnsw.Search(store.Vector(row), 10, 64, nullptr, &stats), exact.Search(store.Vector(row), 10));
    queries++;
  }
  EXPECT_GT(recall / queries, 0.95);
  EXPECT_LT(stats.scored / static_cast<size_t>(queries), store.Size() / 2);  // far less than a scan
}

TEST(P14_6_HnswIndex, BuildIsReproducibleAndEfRaisesRecall) {
  MemDictionary dict;
  VectorStore store(32);
  Blobs(&store, &dict, 30, 60, 0.9f, 2);
  HnswIndex a(&store, {.m = 8, .ef_construction = 40, .seed = 5});
  HnswIndex b(&store, {.m = 8, .ef_construction = 40, .seed = 5});
  a.Build();
  b.Build();
  EXPECT_EQ(a.Bytes(), b.Bytes());
  EXPECT_EQ(a.TopLayer(), b.TopLayer());
  FlatIndex exact(&store, FlatIndex::Precision::kFloat32);
  double prev = 0;
  for (const uint32_t ef : {10u, 40u, 200u}) {
    double recall = 0;
    int queries = 0;
    for (uint32_t row = 0; row < store.Size(); row += 23) {
      recall += Recall(a.Search(store.Vector(row), 10, ef), exact.Search(store.Vector(row), 10));
      queries++;
    }
    EXPECT_GE(recall / queries, prev - 0.02) << ef;
    prev = recall / queries;
  }
  EXPECT_GT(prev, 0.9);
}

TEST(P14_6_HnswIndex, MaskReturnsOnlyAllowedRows) {
  MemDictionary dict;
  VectorStore store(32);
  Blobs(&store, &dict, 20, 60, 0.5f, 3);
  HnswIndex hnsw(&store, {.m = 16, .ef_construction = 100});
  hnsw.Build();
  FlatIndex exact(&store, FlatIndex::Precision::kFloat32);
  RowMask allowed(store.Size(), 0);
  std::vector<uint32_t> rows;
  for (uint32_t row = 0; row < store.Size(); row++) {
    if (row % 4 == 0) {
      allowed[row] = 1;
      rows.push_back(row);
    }
  }
  const RowFilter allowed_filter(&allowed);
  double recall = 0;
  int queries = 0;
  for (uint32_t row = 1; row < store.Size(); row += 31) {
    const auto got = hnsw.Search(store.Vector(row), 10, 64, &allowed_filter);
    for (const auto& n : got) {
      ASSERT_EQ(*store.Row(n.id) % 4, 0u);
    }
    recall += Recall(got, exact.SearchRows(store.Vector(row), 10, rows));
    queries++;
  }
  EXPECT_GT(recall / queries, 0.85);
  // Nothing allowed: nothing returned, and the walk ends.
  const RowMask none(store.Size(), 0);
  const RowFilter none_filter(&none);
  EXPECT_TRUE(hnsw.Search(store.Vector(0), 5, 32, &none_filter).empty());
}

}  // namespace
}  // namespace ontodb
