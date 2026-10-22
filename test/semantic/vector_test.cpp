#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>

#include <gtest/gtest.h>

#include "common/exception.h"
#include "dictionary/mem_dictionary.h"
#include "semantic/flat_index.h"
#include "semantic/vector_store.h"
#include "test_util.h"

namespace ontodb {
namespace {

auto RandomVectors(VectorStore* store, MemDictionary* dict, size_t n, uint64_t seed) -> void {
  std::mt19937_64 rng(seed);
  std::normal_distribution<float> gauss;
  std::vector<float> v(store->Dim());
  for (size_t i = 0; i < n; i++) {
    for (float& x : v) {
      x = gauss(rng);
    }
    store->Put(dict->Insert("<t" + std::to_string(i) + ">"), v.data());
  }
}

TEST(P14_3_VectorStore, PutNormalizesAndReplaces) {
  VectorStore store(3);
  const float v[3] = {3, 0, 4};
  store.Put(7, v);
  const float* row = store.Vector(*store.Row(7));
  EXPECT_FLOAT_EQ(row[0], 0.6f);
  EXPECT_FLOAT_EQ(row[2], 0.8f);
  const float w[3] = {0, 2, 0};
  store.Put(7, w);
  EXPECT_EQ(store.Size(), 1u);
  EXPECT_FLOAT_EQ(store.Vector(*store.Row(7))[1], 1.0f);
  EXPECT_FALSE(store.Row(8).has_value());
  const float zero[3] = {0, 0, 0};
  EXPECT_THROW(store.Put(9, zero), std::invalid_argument);
  const float bad[3] = {1, NAN, 0};
  EXPECT_THROW(store.Put(9, bad), std::invalid_argument);
}

TEST(P14_3_VectorStore, SaveLoadRoundTripThroughASecondDictionary) {
  test::TempDir dir;
  const std::string path = (dir.path() / "v.ovec").string();
  MemDictionary a;
  VectorStore src(8);
  RandomVectors(&src, &a, 50, 1);
  src.Save(path, a);

  // A dictionary that numbers the same spellings differently.
  MemDictionary b;
  for (int i = 49; i >= 0; i--) {
    b.Insert("<t" + std::to_string(i) + ">");
  }
  b.Insert("<other>");
  VectorStore dst(8);
  const auto stats = dst.Load(path, &b);
  EXPECT_EQ(stats.read, 50u);
  EXPECT_EQ(stats.stored, 50u);
  EXPECT_EQ(stats.skipped_unknown, 0u);
  for (size_t i = 0; i < 50; i++) {
    const std::string name = "<t" + std::to_string(i) + ">";
    const float* x = src.Vector(*src.Row(*a.Lookup(name)));
    const float* y = dst.Vector(*dst.Row(*b.Lookup(name)));
    for (int d = 0; d < 8; d++) {
      EXPECT_EQ(x[d], y[d]);
    }
  }
  // Unknown spellings are skipped, or interned on request.
  MemDictionary empty;
  VectorStore skipped(8), interned(8);
  EXPECT_EQ(skipped.Load(path, &empty).skipped_unknown, 50u);
  EXPECT_EQ(interned.Load(path, &empty, true).stored, 50u);
  EXPECT_EQ(empty.Size(), 50u);
}

TEST(P14_3_VectorStore, RejectsCorruptionAndWrongDimension) {
  test::TempDir dir;
  const std::string path = (dir.path() / "v.ovec").string();
  MemDictionary a;
  VectorStore src(8);
  RandomVectors(&src, &a, 5, 2);
  src.Save(path, a);

  VectorStore wrong(4);
  EXPECT_THROW(wrong.Load(path, &a), StorageException);

  std::string bytes;
  {
    std::ifstream in(path, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  std::string flipped = bytes;
  flipped[flipped.size() / 2] ^= 0x10;
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << flipped;
  }
  VectorStore dst(8);
  EXPECT_THROW(dst.Load(path, &a), StorageException);
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes.substr(0, bytes.size() - 9);
  }
  EXPECT_THROW(dst.Load(path, &a), StorageException);
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "not a vector file";
  }
  EXPECT_THROW(dst.Load(path, &a), StorageException);
}

TEST(P14_4_FlatIndex, ExactMatchesBruteForce) {
  MemDictionary dict;
  VectorStore store(16);
  RandomVectors(&store, &dict, 300, 3);
  FlatIndex flat(&store, FlatIndex::Precision::kFloat32);
  std::mt19937_64 rng(9);
  std::normal_distribution<float> gauss;
  for (int t = 0; t < 20; t++) {
    std::vector<float> q(16);
    for (float& x : q) {
      x = gauss(rng) * 3;  // not normalized on purpose
    }
    const auto qn = Normalized(q.data(), 16);
    std::vector<std::pair<float, term_id_t>> all;
    for (uint32_t row = 0; row < store.Size(); row++) {
      float s = 0;
      for (int d = 0; d < 16; d++) {
        s += qn[d] * store.Vector(row)[d];
      }
      all.emplace_back(s, store.IdAt(row));
    }
    std::sort(all.rbegin(), all.rend());
    SearchStats stats;
    const auto got = flat.Search(q.data(), 10, &stats);
    EXPECT_EQ(stats.scored, 300u);
    ASSERT_EQ(got.size(), 10u);
    for (size_t i = 0; i < 10; i++) {
      EXPECT_EQ(got[i].id, all[i].second);
      EXPECT_NEAR(got[i].score, all[i].first, 1e-5);
    }
  }
  EXPECT_TRUE(flat.Search(store.Vector(0), 0).empty());
  // A stored vector's nearest neighbor is itself, with score 1.
  const auto self = flat.Search(store.Vector(5), 1);
  EXPECT_EQ(self[0].id, store.IdAt(5));
  EXPECT_NEAR(self[0].score, 1.0f, 1e-5);
}

TEST(P14_4_FlatIndex, SearchRowsRestrictsTheCandidates) {
  MemDictionary dict;
  VectorStore store(8);
  RandomVectors(&store, &dict, 100, 4);
  FlatIndex flat(&store, FlatIndex::Precision::kFloat32);
  std::vector<uint32_t> rows;
  for (uint32_t row = 0; row < 100; row += 3) {
    rows.push_back(row);
  }
  SearchStats stats;
  const auto got = flat.SearchRows(store.Vector(1), 5, rows, &stats);
  EXPECT_EQ(stats.scored, rows.size());
  for (const auto& n : got) {
    EXPECT_EQ(*store.Row(n.id) % 3, 0u);
  }
  EXPECT_EQ(flat.SearchRows(store.Vector(1), 5, {}).size(), 0u);
}

TEST(P14_4_FlatIndex, Sq8ErrorIsWithinTheBound) {
  MemDictionary dict;
  VectorStore store(64);
  RandomVectors(&store, &dict, 200, 5);
  FlatIndex exact(&store, FlatIndex::Precision::kFloat32);
  FlatIndex sq8(&store, FlatIndex::Precision::kSq8);
  EXPECT_LT(sq8.Bytes(), exact.Bytes() / 3);
  for (uint32_t qrow = 0; qrow < 10; qrow++) {
    const float* q = store.Vector(qrow);
    for (uint32_t row = 0; row < 200; row++) {
      float peak = 0;
      for (int d = 0; d < 64; d++) {
        peak = std::max(peak, std::fabs(store.Vector(row)[d]));
      }
      const float bound = (peak / 127.0f / 2.0f) * std::sqrt(64.0f) + 1e-5f;
      EXPECT_LE(std::fabs(sq8.Score(q, row) - exact.Score(q, row)), bound);
    }
  }
}

}  // namespace
}  // namespace ontodb
