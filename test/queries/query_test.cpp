#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <sstream>

#include "common/database.h"
#include "test_util.h"

namespace ontodb {
namespace {

struct QueryCase {
  std::string name;
  bool update{false};
  bool ordered{false};
  std::string text;
  std::vector<std::string> expect;
};

struct Script {
  std::filesystem::path data;
  std::vector<QueryCase> cases;
};

auto JoinRow(const std::vector<std::string>& row) -> std::string {
  std::string line;
  for (size_t i = 0; i < row.size(); i++) {
    if (i != 0) {
      line.push_back('\t');
    }
    line += row[i];
  }
  return line;
}

auto LoadScript(const std::filesystem::path& path) -> Script {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("cannot read " + path.string());
  }
  Script script;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    if (line.rfind("data ", 0) == 0) {
      script.data = test::DataFile(line.substr(5));
      continue;
    }
    if (line.rfind("query", 0) != 0 && line.rfind("update", 0) != 0) {
      continue;
    }
    QueryCase item;
    item.update = line.rfind("update", 0) == 0;
    item.ordered = line.rfind("query order", 0) == 0;
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
      item.expect.push_back(line);
    }
    script.cases.push_back(std::move(item));
  }
  return script;
}

void CheckCase(Database& db, const QueryCase& item) {
  const auto result = db.Execute(item.text, nullptr);
  if (item.update) {
    ASSERT_EQ(item.expect.size(), 1) << item.name;
    EXPECT_EQ(result.message, item.expect[0]) << item.name;
    return;
  }
  std::vector<std::string> got;
  got.reserve(result.rows.size());
  for (const auto& row : result.rows) {
    got.push_back(JoinRow(row));
  }
  auto expect = item.expect;
  if (!item.ordered && item.text.find("ORDER BY") == std::string::npos &&
      item.text.find("order by") == std::string::npos) {
    std::sort(got.begin(), got.end());
    std::sort(expect.begin(), expect.end());
  }
  EXPECT_EQ(got, expect) << item.name << "\n" << item.text;
}

void RunFile(const char* filename) {
  const auto script = LoadScript(test::SourceDir() / "test" / "queries" / filename);
  Database db;
  ASSERT_FALSE(script.data.empty());
  db.Load(script.data.string());
  for (const auto& item : script.cases) {
    CheckCase(db, item);
  }
}

TEST(QueryHarness, TinyUniversity) {
  RunFile("tiny.test");
}

TEST(QueryHarness, PizzaSubset) {
  RunFile("pizza.test");
}

}  // namespace
}  // namespace ontodb
