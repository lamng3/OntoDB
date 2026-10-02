#include "loader/rdf_loader.h"

#include <gtest/gtest.h>

#include "dictionary/mem_dictionary.h"
#include "store/mem_store.h"
#include "test_util.h"

namespace ontodb {
namespace {

TEST(RdfLoader, LoadsTinyUniversity) {
  MemDictionary dict;
  MemStore store;
  const auto stats = RdfLoader().Load(test::DataFile("tiny.ttl"), dict, store);
  EXPECT_EQ(stats.triples, 53);
  EXPECT_EQ(store.Size(), 53);
  EXPECT_GT(dict.Size(), 10);
  EXPECT_GE(stats.milliseconds, 0);
}

TEST(RdfLoader, RejectsUnknownExtension) {
  MemDictionary dict;
  MemStore store;
  EXPECT_THROW(RdfLoader().Load(test::DataFile("tiny.ttl").replace_extension(".xyz"), dict, store),
               std::runtime_error);
}

}  // namespace
}  // namespace ontodb
