#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <sstream>

#include "binder/binder.h"
#include "buffer/buffer_pool_manager.h"
#include "catalog/catalog.h"
#include "common/database.h"
#include "common/exception.h"
#include "dictionary/disk_dictionary.h"
#include "execution/execution_engine.h"
#include "loader/rdf_loader.h"
#include "parser/parser.h"
#include "planner/planner.h"
#include "storage/disk/disk_manager.h"
#include "store/indexed_store.h"
#include "test_util.h"

namespace ontodb {
namespace {

struct Case {
  std::string name;
  std::string text;
};

auto LoadCases(const std::filesystem::path& path, std::string* data) -> std::vector<Case> {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("cannot read " + path.string());
  }
  std::vector<Case> cases;
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("data ", 0) == 0) {
      *data = line.substr(5);
      continue;
    }
    if (line.rfind("query", 0) != 0 && line.rfind("update", 0) != 0) {
      continue;
    }
    Case item;
    item.name = line;
    std::string body;
    while (std::getline(in, line) && line != "----") {
      if (!body.empty()) {
        body.push_back('\n');
      }
      body += line;
    }
    item.text = body;
    while (std::getline(in, line) && !line.empty()) {
    }
    cases.push_back(std::move(item));
  }
  return cases;
}

auto JoinRows(const std::vector<std::vector<std::string>>& rows) -> std::vector<std::string> {
  std::vector<std::string> lines;
  for (const auto& row : rows) {
    std::string line;
    for (size_t i = 0; i < row.size(); i++) {
      if (i != 0) {
        line.push_back('\t');
      }
      line += row[i];
    }
    lines.push_back(std::move(line));
  }
  std::sort(lines.begin(), lines.end());
  return lines;
}

TEST(Differential, MemAndIndexed) {
  try {
    for (const char* file : {"tiny.test", "pizza.test"}) {
      test::TempDir dir;
      DiskManager disk((dir.path() / "db").string());
      BufferPoolManager bpm(8, &disk, 2);
      Catalog catalog(&bpm);
      DiskDictionary dictionary(&bpm, &catalog);
      IndexedStore indexed(&catalog, &bpm, &dictionary);
      Config config;
      IoStats stats;
      NullTraceSink trace;
      ExecutorContext ctx(&indexed, &dictionary, &config, nullptr, nullptr, nullptr, &bpm, &stats,
                          &trace, nullptr);
      ExecutionEngine engine(&ctx);
      std::string data;
      const auto cases = LoadCases(test::SourceDir() / "test" / "queries" / file, &data);
      Database mem;
      mem.Load(test::DataFile(data).string());
      RdfLoader().Load(test::DataFile(data), dictionary, indexed);
      for (const auto& item : cases) {
        const auto left = mem.Execute(item.text, nullptr);
        const auto bound = Binder(dictionary).Bind(Parser(item.text).Parse());
        const auto right = engine.Execute(bound);
        EXPECT_EQ(JoinRows(left.rows), JoinRows(right.rows)) << item.name;
        if (item.name.rfind("update", 0) == 0) {
          EXPECT_EQ(left.message, right.message) << item.name;
        }
        EXPECT_EQ(bpm.GetPinnedFrameCount(), 0u) << item.name;
      }
    }
  } catch (const NotImplementedException& ex) {
    GTEST_SKIP() << ex.what();
  }
}

}  // namespace
}  // namespace ontodb
