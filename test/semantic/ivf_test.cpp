#include <algorithm>
#include <cmath>
#include <random>
#include <set>

#include <gtest/gtest.h>

#include "dictionary/mem_dictionary.h"
#include "semantic/flat_index.h"
#include "semantic/ivf_index.h"
#include "semantic/kmeans.h"
#include "semantic/product_quantizer.h"
#include "storage/env.h"

namespace ontodb {
namespace {

// `clusters` Gaussian blobs of `per` points each.
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

TEST(P14_5_KMeans, SeparatesBlobsAndIsReproducible) {
  MemDictionary dict;
  VectorStore store(8);
  Blobs(&store, &dict, 4, 50, 0.05f, 1);
  const auto a = KMeans(store.Vector(0), store.Size(), 8, 4, 10, 7, true);
  const auto b = KMeans(store.Vector(0), store.Size(), 8, 4, 10, 7, true);
  EXPECT_EQ(a, b);
  // Every blob lands in one center.
  std::set<uint32_t> used;
  for (uint32_t c = 0; c < 4; c++) {
    std::set<uint32_t> in_blob;
    for (uint32_t i = 0; i < 50; i++) {
      in_blob.insert(NearestCenter(a.data(), 4, 8, store.Vector(c * 50 + i), true));
    }
    EXPECT_EQ(in_blob.size(), 1u) << c;
    used.insert(*in_blob.begin());
  }
  EXPECT_EQ(used.size(), 4u);
  EXPECT_EQ(KMeans(store.Vector(0), 3, 8, 5, 3, 1, true).size(), 40u);  // n < k still returns k centers
}

TEST(P14_5_ProductQuantizer, AdcEqualsDotWithTheDecodedVector) {
  MemDictionary dict;
  VectorStore store(32);
  Blobs(&store, &dict, 8, 100, 0.3f, 2);
  ProductQuantizer pq(32, 8);
  pq.Train(store.Vector(0), store.Size(), 8, 1);
  std::vector<uint8_t> code(8);
  std::vector<float> decoded(32);
  double err = 0;
  for (uint32_t row = 0; row < 50; row++) {
    pq.Encode(store.Vector(row), code.data());
    pq.Decode(code.data(), decoded.data());
    const auto table = pq.Table(store.Vector(7));
    float want = 0;
    for (int d = 0; d < 32; d++) {
      want += store.Vector(7)[d] * decoded[d];
    }
    EXPECT_NEAR(ProductQuantizer::Score(table, code.data(), 8), want, 1e-4);
    for (int d = 0; d < 32; d++) {
      err += std::fabs(decoded[d] - store.Vector(row)[d]);
    }
  }
  EXPECT_LT(err / (50 * 32), 0.1);
  EXPECT_THROW(ProductQuantizer(30, 8), std::invalid_argument);
}

TEST(P14_5_IvfIndex, EveryRowIsInExactlyOneList) {
  test::DiskEnv env(64);
  MemDictionary dict;
  VectorStore store(16);
  Blobs(&store, &dict, 10, 60, 0.2f, 3);
  IvfIndex ivf(&store, &env.bpm, {.lists = 12});
  ivf.Build();
  EXPECT_EQ(ivf.Lists(), 12u);
  std::set<uint32_t> rows;
  size_t keys = 0;
  for (auto it = ivf.Tree()->Begin(); !it.IsEnd(); ++it) {
    term_id_t list = 0, row = 0, unused = 0;
    (*it).Decode(&list, &row, &unused);
    EXPECT_LT(list, 12u);
    rows.insert(static_cast<uint32_t>(row));
    keys++;
  }
  EXPECT_EQ(keys, 600u);
  EXPECT_EQ(rows.size(), 600u);
  EXPECT_EQ(ivf.Tree()->CheckInvariants(), std::nullopt);
}

TEST(P14_5_IvfIndex, ProbingEveryListIsExact) {
  test::DiskEnv env(64);
  MemDictionary dict;
  VectorStore store(16);
  Blobs(&store, &dict, 10, 60, 0.4f, 4);
  FlatIndex exact(&store, FlatIndex::Precision::kFloat32);
  IvfIndex ivf(&store, &env.bpm, {.lists = 16});
  ivf.Build();
  for (uint32_t row = 0; row < 600; row += 37) {
    const auto want = exact.Search(store.Vector(row), 10);
    SearchStats stats;
    const auto got = ivf.Search(store.Vector(row), 10, 16, nullptr, &stats);
    ASSERT_EQ(got.size(), 10u);
    EXPECT_EQ(Recall(got, want), 1.0);
    EXPECT_EQ(stats.scored, 600u);
    EXPECT_EQ(stats.lists, 16u);
  }
}

TEST(P14_5_IvfIndex, RecallRisesWithProbesAndWorkFalls) {
  test::DiskEnv env(64);
  MemDictionary dict;
  VectorStore store(32);
  Blobs(&store, &dict, 40, 50, 0.5f, 5);
  FlatIndex exact(&store, FlatIndex::Precision::kFloat32);
  IvfIndex ivf(&store, &env.bpm, {.lists = 40});
  ivf.Build();
  double prev = 0;
  size_t prev_scored = 0;
  for (const uint32_t nprobe : {1u, 4u, 16u, 40u}) {
    double recall = 0;
    SearchStats stats;
    int queries = 0;
    for (uint32_t row = 0; row < store.Size(); row += 41) {
      recall += Recall(ivf.Search(store.Vector(row), 10, nprobe, nullptr, &stats), exact.Search(store.Vector(row), 10));
      queries++;
    }
    recall /= queries;
    EXPECT_GE(recall, prev - 1e-9) << nprobe;
    EXPECT_GE(stats.scored, prev_scored);
    prev = recall;
    prev_scored = stats.scored;
  }
  EXPECT_EQ(prev, 1.0);
}

TEST(P14_5_IvfIndex, MaskSkipsRowsBeforeScoring) {
  test::DiskEnv env(64);
  MemDictionary dict;
  VectorStore store(16);
  Blobs(&store, &dict, 5, 40, 0.3f, 6);
  IvfIndex ivf(&store, &env.bpm, {.lists = 8});
  ivf.Build();
  RowMask allowed(store.Size(), 0);
  for (uint32_t row = 0; row < store.Size(); row += 2) {
    allowed[row] = 1;
  }
  const RowFilter allowed_filter(&allowed);
  SearchStats stats;
  const auto got = ivf.Search(store.Vector(3), 5, 8, &allowed_filter, &stats);
  EXPECT_EQ(got.size(), 5u);
  for (const auto& n : got) {
    EXPECT_EQ(*store.Row(n.id) % 2, 0u);
  }
  EXPECT_EQ(stats.scored, 100u);
  EXPECT_EQ(stats.keys, 200u);
}

TEST(P14_5_IvfIndex, UnderAFilterItKeepsProbingUntilItHasEnough) {
  test::DiskEnv env(64);
  MemDictionary dict;
  VectorStore store(16);
  Blobs(&store, &dict, 10, 40, 0.2f, 8);
  IvfIndex ivf(&store, &env.bpm, {.lists = 10});
  ivf.Build();
  // Allow only the last blob. A query from the first blob finds nothing allowed
  // in its nearest list, so one probe is not enough.
  RowMask allowed(store.Size(), 0);
  for (uint32_t row = 360; row < 400; row++) {
    allowed[row] = 1;
  }
  const RowFilter filter(&allowed);
  SearchStats stats;
  const auto got = ivf.Search(store.Vector(0), 5, 1, &filter, &stats);
  EXPECT_EQ(got.size(), 5u);
  EXPECT_GT(stats.lists, 1u);
  for (const auto& n : got) {
    EXPECT_GE(*store.Row(n.id), 360u);
  }
  // Without a filter the same call probes exactly one list.
  SearchStats plain;
  ivf.Search(store.Vector(0), 5, 1, nullptr, &plain);
  EXPECT_EQ(plain.lists, 1u);
}

TEST(P14_5_IvfIndex, Sq8AndPqKeepMostOfTheRecall) {
  test::DiskEnv env(128);
  MemDictionary dict;
  VectorStore store(32);
  Blobs(&store, &dict, 30, 60, 0.6f, 7);
  FlatIndex exact(&store, FlatIndex::Precision::kFloat32);
  for (const auto encoding : {IvfIndex::Encoding::kSq8, IvfIndex::Encoding::kPq}) {
    IvfIndex ivf(&store, &env.bpm, {.lists = 30, .encoding = encoding, .pq_m = 16});
    ivf.Build();
    double recall = 0;
    int queries = 0;
    for (uint32_t row = 0; row < store.Size(); row += 29) {
      recall += Recall(ivf.Search(store.Vector(row), 10, 30), exact.Search(store.Vector(row), 10));
      queries++;
    }
    EXPECT_GT(recall / queries, encoding == IvfIndex::Encoding::kSq8 ? 0.95 : 0.6) << static_cast<int>(encoding);
  }
}

}  // namespace
}  // namespace ontodb
