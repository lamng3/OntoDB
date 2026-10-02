#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <sstream>

#include "binder/binder.h"
#include "catalog/catalog.h"
#include "dictionary/disk_dictionary.h"
#include "execution/execution_engine.h"
#include "loader/rdf_loader.h"
#include "parser/parser.h"
#include "storage/env.h"
#include "store/indexed_store.h"
#include "test_util.h"

namespace ontodb {
namespace {

auto ReadFile(const std::filesystem::path& path) -> std::string {
  std::ifstream in(path);
  EXPECT_TRUE(in) << path;
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

TEST(LubmQueries, ParseAll) {
  const auto dir = test::SourceDir() / "data" / "lubm" / "queries";
  for (int i = 1; i <= 14; i++) {
    char name[16];
    std::snprintf(name, sizeof(name), "q%02d.sparql", i);
    const auto text = ReadFile(dir / name);
    EXPECT_NO_THROW(Parser(text).Parse()) << name;
  }
}

TEST(P6_2_Lubm, DISABLED_IndexedUnderSmallPool) {
  const auto owl = test::SourceDir() / "data" / "downloads" / "lubm" / "University0_0.owl";
  test::DiskEnv env(32, 2);
  Catalog catalog(&env.bpm);
  DiskDictionary dictionary(&env.bpm, &catalog);
  IndexedStore store(&catalog, &env.bpm, &dictionary);
  ASSERT_TRUE(std::filesystem::exists(owl)) << "run scripts/fetch-data lubm";
  RdfLoader().Load(owl, dictionary, store);
  const auto dir = test::SourceDir() / "data" / "lubm" / "queries";
  for (const char* name : {"q01.sparql", "q06.sparql", "q14.sparql"}) {
    const auto text = ReadFile(dir / name);
    Config config;
    IoStats stats;
    NullTraceSink trace;
    ExecutorContext ctx(&store, &dictionary, &config, nullptr, nullptr, nullptr, &env.bpm, &stats,
                        &trace, nullptr);
    ExecutionEngine engine(&ctx);
    const auto result = engine.Execute(Binder(dictionary).Bind(Parser(text).Parse()));
    EXPECT_GT(result.rows.size(), 0u) << name;
  }
  EXPECT_EQ(env.bpm.GetPinnedFrameCount(), 0u);
}

}  // namespace
}  // namespace ontodb
