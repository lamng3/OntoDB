#include "store/mem_store.h"

#include <gtest/gtest.h>

namespace ontodb {
namespace {

auto T(term_id_t s, term_id_t p, term_id_t o) -> Triple {
  return Triple{s, p, o};
}

auto Collect(TripleStore& store, const TriplePattern& pattern) -> std::vector<Triple> {
  auto iter = store.Scan(pattern);
  iter->Init();
  std::vector<Triple> out;
  Triple triple;
  while (iter->Next(&triple)) {
    out.push_back(triple);
  }
  iter->Init();
  std::vector<Triple> again;
  while (iter->Next(&triple)) {
    again.push_back(triple);
  }
  EXPECT_EQ(out, again);
  return out;
}

TEST(MemStore, InsertDeleteAndDuplicate) {
  MemStore store;
  EXPECT_TRUE(store.Insert(T(1, 2, 3)));
  EXPECT_FALSE(store.Insert(T(1, 2, 3)));
  EXPECT_EQ(store.Size(), 1);
  EXPECT_TRUE(store.Delete(T(1, 2, 3)));
  EXPECT_FALSE(store.Delete(T(1, 2, 3)));
  EXPECT_EQ(store.Size(), 0);
  EXPECT_TRUE(Collect(store, {}).empty());
}

TEST(MemStore, AllEightBoundPatterns) {
  MemStore store;
  store.Insert(T(1, 2, 3));
  store.Insert(T(1, 2, 4));
  store.Insert(T(1, 9, 3));
  store.Insert(T(8, 2, 3));

  EXPECT_EQ(Collect(store, {}).size(), 4);
  EXPECT_EQ(Collect(store, {1, std::nullopt, std::nullopt}).size(), 3);
  EXPECT_EQ(Collect(store, {std::nullopt, 2, std::nullopt}).size(), 3);
  EXPECT_EQ(Collect(store, {std::nullopt, std::nullopt, 3}).size(), 3);
  EXPECT_EQ(Collect(store, {1, 2, std::nullopt}).size(), 2);
  EXPECT_EQ(Collect(store, {1, std::nullopt, 3}).size(), 2);
  EXPECT_EQ(Collect(store, {std::nullopt, 2, 3}).size(), 2);
  auto full = Collect(store, {1, 2, 3});
  ASSERT_EQ(full.size(), 1);
  EXPECT_EQ(full[0], T(1, 2, 3));
  EXPECT_TRUE(Collect(store, {7, 7, 7}).empty());
}

}  // namespace
}  // namespace ontodb
