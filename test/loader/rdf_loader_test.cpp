#include "loader/rdf_loader.h"

#include <gtest/gtest.h>

#include "common/exception.h"
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

// A store failure reaches the caller with its type. The loader parses the
// whole file first (terms go to the dictionary as they are read) and then
// hands the triples to the store in one batch; an exception thrown inside
// raptor's callback (a dictionary failure) is caught there, the parse is
// aborted, and it is rethrown after raptor is freed.
TEST(RdfLoader, StoreFailureAbortsTheParseAndKeepsItsType) {
  class FailingStore : public TripleStore {
   public:
    auto Insert(const Triple&) -> bool override {
      if (++inserts == 3) {
        throw NotImplementedException("PLAN 3.4: fake store");
      }
      return true;
    }
    auto Delete(const Triple&) -> bool override { return false; }
    auto Scan(const TriplePattern&) -> std::unique_ptr<TripleIterator> override { return nullptr; }
    int inserts{0};
  } store;
  MemDictionary dict;
  EXPECT_THROW(RdfLoader().Load(test::DataFile("tiny.ttl"), dict, store), NotImplementedException);
  EXPECT_EQ(store.inserts, 3);  // no statement handled after the failure
}

}  // namespace
}  // namespace ontodb
