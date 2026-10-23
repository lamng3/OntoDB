#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <set>
#include <stdexcept>
#include <tuple>
#include <vector>

#include "common/exception.h"
#include "storage/env.h"
#include "storage/index/b_plus_tree.h"
#include "storage/page/b_plus_tree_header_page.h"
#include "storage/page/b_plus_tree_internal_page.h"
#include "storage/page/b_plus_tree_leaf_page.h"

namespace ontodb {
namespace {

auto Header(BufferPoolManager* bpm) -> page_id_t {
  page_id_t id = INVALID_PAGE_ID;
  Page* page = bpm->NewPage(&id);
  EXPECT_NE(page, nullptr);
  BPlusTreeHeaderPage header(page);
  header.Init();
  header.SetRootPageId(INVALID_PAGE_ID);
  EXPECT_TRUE(bpm->UnpinPage(id, true));
  EXPECT_EQ(bpm->GetPinnedFrameCount(), 0u);
  return id;
}

auto Scan(BPlusTree* tree) -> std::vector<TripleKey> {
  std::vector<TripleKey> keys;
  for (auto it = tree->Begin(); !it.IsEnd(); ++it) {
    keys.push_back(*it);
  }
  return keys;
}

void ExpectSame(const std::vector<TripleKey>& got, const std::vector<TripleKey>& expect) {
  ASSERT_EQ(got.size(), expect.size());
  for (size_t i = 0; i < got.size(); i++) {
    EXPECT_EQ(got[i].Compare(expect[i]), 0) << i;
  }
}

TEST(P2_1_PageLayout, LeafInternalAndHeader) {
  Page raw;
  EXPECT_GE(BPlusTreeLeafPage::MaxSize(), 2);
  EXPECT_GE(BPlusTreeInternalPage::MaxSize(), 2);
  BPlusTreeLeafPage leaf(&raw);
  leaf.Init(BPlusTreeLeafPage::MaxSize());
  EXPECT_TRUE(leaf.IsLeaf());
  EXPECT_EQ(leaf.GetSize(), 0);
  EXPECT_EQ(leaf.GetNextPageId(), INVALID_PAGE_ID);
  leaf.SetNextPageId(12);
  EXPECT_EQ(leaf.GetNextPageId(), 12);
  TripleKey key;
  key.Data()[0] = static_cast<uint8_t>(0xAB);
  key.Data()[23] = 7;
  leaf.SetKeyAt(0, key);
  leaf.SetSize(1);
  EXPECT_EQ(leaf.KeyAt(0).Data()[0], static_cast<uint8_t>(0xAB));
  EXPECT_EQ(leaf.KeyAt(0).Data()[23], 7);
  leaf.SetLSN(42);
  EXPECT_EQ(leaf.GetLSN(), 42);

  Page internal_raw;
  BPlusTreeInternalPage internal(&internal_raw);
  internal.Init(BPlusTreeInternalPage::MaxSize());
  EXPECT_FALSE(internal.IsLeaf());
  internal.SetValueAt(0, 3);
  internal.SetKeyAt(1, key);
  internal.SetValueAt(1, 4);
  internal.SetSize(1);
  EXPECT_EQ(internal.ValueAt(0), 3);
  EXPECT_EQ(internal.KeyAt(1).Data()[0], static_cast<uint8_t>(0xAB));
  EXPECT_EQ(internal.ValueAt(1), 4);

  Page header_raw;
  BPlusTreeHeaderPage header(&header_raw);
  header.Init();
  EXPECT_EQ(header.GetRootPageId(), INVALID_PAGE_ID);
  header.SetRootPageId(8);
  EXPECT_EQ(header.GetRootPageId(), 8);
}

TEST(P2_2_TreeDebug, ToDotAndInvariants) {
  test::DiskEnv env;
  BPlusTree tree("spo", Header(&env.bpm), &env.bpm);
  EXPECT_FALSE(tree.CheckInvariants().has_value()) << tree.CheckInvariants().value_or("");
  EXPECT_FALSE(tree.ToString().empty());
  tree.Insert(TripleKey::Encode(1, 1, 1));
  tree.Insert(TripleKey::Encode(1, 1, 2));
  const auto dot = tree.ToDot();
  EXPECT_NE(dot.find("digraph"), std::string::npos);
  EXPECT_FALSE(tree.CheckInvariants().has_value());
  EXPECT_EQ(env.bpm.GetPinnedFrameCount(), 0u);
}

TEST(P2_3_PointSearch, FindsAndMisses) {
  test::DiskEnv env;
  BPlusTree tree("spo", Header(&env.bpm), &env.bpm);
  EXPECT_TRUE(tree.IsEmpty());
  const auto key = TripleKey::Encode(3, 4, 5);
  EXPECT_FALSE(tree.GetValue(key));
  EXPECT_TRUE(tree.Insert(key));
  EXPECT_FALSE(tree.IsEmpty());
  EXPECT_TRUE(tree.GetValue(key));
  EXPECT_FALSE(tree.GetValue(TripleKey::Encode(3, 4, 6)));
  EXPECT_EQ(env.bpm.GetPinnedFrameCount(), 0u);
}

TEST(P2_4_Insert, RandomMatchesSortedVector) {
  test::DiskEnv env(32, 2);
  BPlusTree tree("spo", Header(&env.bpm), &env.bpm);
  std::mt19937 rng(42);
  std::vector<TripleKey> keys;
  while (keys.size() < 200) {
    auto key = TripleKey::Encode(1 + rng() % 40, 1 + rng() % 8, 1 + rng() % 40);
    bool seen = false;
    for (const auto& existing : keys) {
      if (existing.Compare(key) == 0) {
        seen = true;
      }
    }
    if (!seen) {
      keys.push_back(key);
    }
  }
  for (const auto& key : keys) {
    EXPECT_TRUE(tree.Insert(key));
    EXPECT_FALSE(tree.Insert(key));
  }
  EXPECT_FALSE(tree.CheckInvariants().has_value());
  auto sorted = keys;
  std::sort(sorted.begin(), sorted.end(),
            [](const TripleKey& a, const TripleKey& b) { return a.Compare(b) < 0; });
  ExpectSame(Scan(&tree), sorted);
  EXPECT_EQ(env.bpm.GetPinnedFrameCount(), 0u);
  for (size_t i = 0; i < keys.size(); i += 2) {
    EXPECT_TRUE(tree.Remove(keys[i]));
    EXPECT_FALSE(tree.Remove(keys[i]));
  }
  std::vector<TripleKey> left;
  for (size_t i = 1; i < keys.size(); i += 2) {
    left.push_back(keys[i]);
  }
  std::sort(left.begin(), left.end(),
            [](const TripleKey& a, const TripleKey& b) { return a.Compare(b) < 0; });
  ExpectSame(Scan(&tree), left);
  EXPECT_FALSE(tree.CheckInvariants().has_value());
  EXPECT_EQ(env.bpm.GetPinnedFrameCount(), 0u);
}

TEST(P2_5_Iterator, PrefixEdgesAndTinyPool) {
  test::DiskEnv env(3, 2);
  BPlusTree tree("spo", Header(&env.bpm), &env.bpm);
  {
    auto it = tree.Begin();
    EXPECT_TRUE(it.IsEnd());
    EXPECT_TRUE(it == tree.End());
  }
  EXPECT_EQ(env.bpm.GetPinnedFrameCount(), 0u);
  EXPECT_TRUE(tree.Insert(TripleKey::Encode(1, 1, 1)));
  {
    auto it = tree.Begin();
    EXPECT_FALSE(it.IsEnd());
    EXPECT_EQ((*it).Compare(TripleKey::Encode(1, 1, 1)), 0);
    ++it;
    EXPECT_TRUE(it.IsEnd());
  }
  EXPECT_EQ(env.bpm.GetPinnedFrameCount(), 0u);
  for (term_id_t object = 1; object <= 400; object++) {
    tree.Insert(TripleKey::Encode(2, 5, object));
  }
  tree.Insert(TripleKey::Encode(3, 1, 1));
  size_t seen = 0;
  size_t max_pins = 0;
  for (auto it = tree.Begin(TripleKey::Encode(2, 5, 0), 2); !it.IsEnd(); ++it) {
    term_id_t subject = 0;
    term_id_t predicate = 0;
    term_id_t object = 0;
    (*it).Decode(&subject, &predicate, &object);
    EXPECT_EQ(subject, 2u);
    EXPECT_EQ(predicate, 5u);
    seen++;
    max_pins = std::max(max_pins, env.bpm.GetPinnedFrameCount());
  }
  EXPECT_EQ(seen, 400u);
  EXPECT_LE(max_pins, 1u);
  EXPECT_EQ(env.bpm.GetPinnedFrameCount(), 0u);
  auto empty = tree.Begin(TripleKey::Encode(9, 9, 0), 1);
  EXPECT_TRUE(empty.IsEnd());
  EXPECT_EQ(env.bpm.GetPinnedFrameCount(), 0u);
}

TEST(P2_5_Iterator, FindFirstAgreesWithTheIterator) {
  // Tiny nodes make many leaves, so a prefix often starts a leaf or follows an emptied one.
  test::DiskEnv env(32, 2);
  BPlusTree tree("spo", Header(&env.bpm), &env.bpm, nullptr, 3, 3);
  std::mt19937 rng(11);
  std::set<std::tuple<term_id_t, term_id_t, term_id_t>> have;
  for (int i = 0; i < 300; i++) {
    const auto k = std::make_tuple<term_id_t, term_id_t, term_id_t>(1 + rng() % 40, 1 + rng() % 5, 1 + rng() % 9);
    if (have.insert(k).second) {
      ASSERT_TRUE(tree.Insert(TripleKey::Encode(std::get<0>(k), std::get<1>(k), std::get<2>(k))));
    }
  }
  int removed = 0;
  for (auto it = have.begin(); it != have.end() && removed < 150;) {
    ASSERT_TRUE(tree.Remove(TripleKey::Encode(std::get<0>(*it), std::get<1>(*it), std::get<2>(*it))));
    it = have.erase(it);
    removed++;
    if (it != have.end()) {
      ++it;
    }
  }
  for (term_id_t a = 0; a <= 42; a++) {
    for (int prefix = 1; prefix <= 2; prefix++) {
      const TripleKey probe = TripleKey::Encode(a, prefix == 2 ? 1 + a % 5 : 0, 0);
      auto it = tree.Begin(probe, prefix);
      const auto got = tree.FindFirst(probe, prefix);
      ASSERT_EQ(got.has_value(), !it.IsEnd()) << a << "/" << prefix;
      if (got.has_value()) {
        EXPECT_EQ(got->Compare(*it), 0) << a << "/" << prefix;
      }
    }
  }
  EXPECT_EQ(env.bpm.GetPinnedFrameCount(), 0u);
}

TEST(P2_6_BulkLoad, MatchesSortedInput) {
  test::DiskEnv env(16, 2);
  std::vector<TripleKey> keys;
  for (term_id_t i = 1; i <= 300; i++) {
    keys.push_back(TripleKey::Encode(i, 1, 1));
  }
  BPlusTree tree("spo", Header(&env.bpm), &env.bpm);
  tree.BulkLoad(keys);
  EXPECT_FALSE(tree.CheckInvariants().has_value());
  ExpectSame(Scan(&tree), keys);
  EXPECT_TRUE(tree.GetValue(keys.front()));
  EXPECT_TRUE(tree.GetValue(keys.back()));
  EXPECT_EQ(env.bpm.GetPinnedFrameCount(), 0u);
}

TEST(P2_7_Delete, MergeOrRedistributeStaysSorted) {
  test::DiskEnv env(16, 2);
  BPlusTree tree("spo", Header(&env.bpm), &env.bpm);
  std::vector<TripleKey> keys;
  for (term_id_t i = 1; i <= 250; i++) {
    keys.push_back(TripleKey::Encode(1, 2, i));
    EXPECT_TRUE(tree.Insert(keys.back()));
  }
  for (size_t i = 0; i < keys.size(); i += 3) {
    EXPECT_TRUE(tree.Remove(keys[i]));
  }
  std::vector<TripleKey> left;
  for (size_t i = 0; i < keys.size(); i++) {
    if (i % 3 != 0) {
      left.push_back(keys[i]);
    }
  }
  ExpectSame(Scan(&tree), left);
  EXPECT_FALSE(tree.CheckInvariants().has_value());
  EXPECT_EQ(env.bpm.GetPinnedFrameCount(), 0u);
}

// Tiny nodes grow deep trees from few keys, so every split, merge, rotation,
// and root change happens many times. Invariants are checked after every
// operation, contents against std::set, and pins after every operation.
TEST(P2_7_Delete, DeepTreeStressMatchesSet) {
  struct Sizes {
    int leaf;
    int internal;
  };
  for (const Sizes sizes : {Sizes{2, 2}, Sizes{3, 3}, Sizes{4, 2}, Sizes{5, 4}}) {
    test::DiskEnv env(256, 2);
    BPlusTree tree("spo", Header(&env.bpm), &env.bpm, nullptr, sizes.leaf, sizes.internal);
    std::set<std::tuple<term_id_t, term_id_t, term_id_t>> model;
    std::mt19937 rng(static_cast<unsigned>(sizes.leaf * 10 + sizes.internal));
    for (int step = 0; step < 700; step++) {
      const term_id_t s = 1 + rng() % 6;
      const term_id_t p = 1 + rng() % 4;
      const term_id_t o = 1 + rng() % 10;
      const auto key = TripleKey::Encode(s, p, o);
      const bool insert = rng() % 3 != 0 || model.empty();
      if (insert) {
        EXPECT_EQ(tree.Insert(key), model.insert({s, p, o}).second);
      } else {
        EXPECT_EQ(tree.Remove(key), model.erase({s, p, o}) == 1);
      }
      const auto broken = tree.CheckInvariants();
      ASSERT_FALSE(broken.has_value()) << "step " << step << ": " << *broken;
      ASSERT_EQ(env.bpm.GetPinnedFrameCount(), 0u) << "step " << step;
    }
    std::vector<TripleKey> expect;
    for (const auto& [s, p, o] : model) {
      expect.push_back(TripleKey::Encode(s, p, o));
    }
    ExpectSame(Scan(&tree), expect);
    for (term_id_t s = 0; s <= 7; s++) {  // every 1-prefix, including absent ones
      size_t want = 0;
      for (const auto& triple : model) {
        want += std::get<0>(triple) == s ? 1 : 0;
      }
      size_t got = 0;
      for (auto it = tree.Begin(TripleKey::Encode(s, 0, 0), 1); !it.IsEnd(); ++it) {
        got++;
      }
      EXPECT_EQ(got, want) << "subject " << s;
    }
    for (const auto& [s, p, o] : model) {  // drain: the tree must end empty
      ASSERT_TRUE(tree.Remove(TripleKey::Encode(s, p, o)));
      ASSERT_FALSE(tree.CheckInvariants().has_value());
    }
    EXPECT_TRUE(tree.IsEmpty());
    EXPECT_EQ(env.bpm.GetPinnedFrameCount(), 0u);
  }
}

TEST(P2_6_BulkLoad, EverySizeKeepsInvariantsWithTinyNodes) {
  for (int n = 0; n <= 60; n++) {
    // 16 frames: after a packed load every node is full, so an insert's
    // crabbing keeps the whole path latched (header, every level, a new page).
    test::DiskEnv env(16, 2);
    BPlusTree tree("spo", Header(&env.bpm), &env.bpm, nullptr, 3, 2);
    std::vector<TripleKey> keys;
    for (int i = 1; i <= n; i++) {
      keys.push_back(TripleKey::Encode(i, 1, 1));
    }
    tree.BulkLoad(keys);
    const auto broken = tree.CheckInvariants();
    ASSERT_FALSE(broken.has_value()) << n << " keys: " << *broken;
    ExpectSame(Scan(&tree), keys);
    if (n > 0) {
      EXPECT_TRUE(tree.Insert(TripleKey::Encode(n + 1, 1, 1)));  // a loaded tree keeps working
      EXPECT_FALSE(tree.CheckInvariants().has_value());
    }
    EXPECT_EQ(env.bpm.GetPinnedFrameCount(), 0u);
  }
  test::DiskEnv env(4, 2);
  BPlusTree tree("spo", Header(&env.bpm), &env.bpm);
  EXPECT_THROW(tree.BulkLoad({TripleKey::Encode(2, 1, 1), TripleKey::Encode(1, 1, 1)}),
               std::invalid_argument);
  tree.Insert(TripleKey::Encode(1, 1, 1));
  EXPECT_THROW(tree.BulkLoad({TripleKey::Encode(2, 1, 1)}), std::logic_error);
}

auto CrashOn(const char* point, bool insert) -> bool {
  test::DiskEnv env(16, 2);
  BPlusTree tree("spo", Header(&env.bpm), &env.bpm);
  if (!insert) {
    for (term_id_t i = 1; i <= 400; i++) {
      tree.Insert(TripleKey::Encode(1, 1, i));
    }
  }
  test::ArmGuard arm(point);
  try {
    if (insert) {
      for (term_id_t i = 1; i <= 400; i++) {
        tree.Insert(TripleKey::Encode(1, 1, i));
      }
    } else {
      for (term_id_t i = 1; i <= 400; i++) {
        tree.Remove(TripleKey::Encode(1, 1, i));
      }
    }
  } catch (const CrashInjected&) {
    return true;
  }
  return false;
}

TEST(P2_4_Insert, LeafSplitCrashPoint) {
  EXPECT_TRUE(CrashOn("BPlusTree::Insert::after_leaf_split", true));
}

TEST(P2_4_Insert, InternalSplitCrashPoint) {
  EXPECT_TRUE(CrashOn("BPlusTree::Insert::after_internal_split", true));
}

TEST(P2_7_Delete, RemoveCrashPoint) {
  const bool merge = CrashOn("BPlusTree::Remove::after_merge", false);
  const bool redistribute = merge || CrashOn("BPlusTree::Remove::after_redistribute", false);
  EXPECT_TRUE(redistribute);
}

}  // namespace
}  // namespace ontodb
