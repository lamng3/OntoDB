#include "dictionary/mem_dictionary.h"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

namespace ontodb {
namespace {

TEST(MemDictionary, InsertLookupBothWays) {
  MemDictionary dict;
  const auto alice = dict.Insert("<http://ex/alice>");
  const auto again = dict.Insert("<http://ex/alice>");
  EXPECT_EQ(alice, again);
  EXPECT_EQ(dict.Size(), 1);
  EXPECT_EQ(dict.Lookup("<http://ex/alice>"), alice);
  EXPECT_EQ(dict.Lookup(alice), "<http://ex/alice>");
  EXPECT_FALSE(dict.Lookup("<http://ex/missing>").has_value());
  EXPECT_FALSE(dict.Lookup(term_id_t{99}).has_value());
}

TEST(MemDictionary, IdsStartAtOneAndIncrease) {
  MemDictionary dict;
  EXPECT_EQ(dict.Size(), 0);
  const auto a = dict.Insert("\"a\"");
  const auto b = dict.Insert("\"b\"");
  EXPECT_EQ(a, 1);
  EXPECT_EQ(b, 2);
}

TEST(MemDictionary, ConcurrentInsertsAgree) {
  MemDictionary dict;
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; t++) {
    threads.emplace_back([&dict, t] {
      for (int i = 0; i < 50; i++) {
        dict.Insert("term-" + std::to_string(t) + "-" + std::to_string(i));
        dict.Insert("shared");
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(dict.Size(), 4 * 50 + 1);
  EXPECT_TRUE(dict.Lookup("shared").has_value());
}

}  // namespace
}  // namespace ontodb
