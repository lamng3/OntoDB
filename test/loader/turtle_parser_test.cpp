#include "loader/turtle_parser.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

#include "dictionary/mem_dictionary.h"
#include "loader/rdf_loader.h"
#include "store/mem_store.h"
#include "test_util.h"

namespace ontodb {
namespace {

using Row = std::vector<std::string>;

auto Parse(const std::string& text) -> std::set<Row> {
  std::set<Row> rows;
  ParseTurtle(text, [&](const std::string& s, const std::string& p, const std::string& o) {
    rows.insert({s, p, o});
  });
  return rows;
}

TEST(TurtleParser, PrefixesPredicateListsAndObjectLists) {
  const auto rows = Parse(
      "@prefix ex: <http://e/> .\n"
      "PREFIX rdfs: <http://www.w3.org/2000/01/rdf-schema#>\n"
      "ex:a a ex:C ; rdfs:label \"A\"@en , \"a\" ;\n ex:n 3 , 2.5 , 1e3 , true .\n");
  EXPECT_EQ(rows.size(), 7u);
  EXPECT_TRUE(rows.count({"<http://e/a>", "<http://www.w3.org/1999/02/22-rdf-syntax-ns#type>", "<http://e/C>"}));
  EXPECT_TRUE(rows.count({"<http://e/a>", "<http://www.w3.org/2000/01/rdf-schema#label>", "\"A\"@en"}));
  EXPECT_TRUE(rows.count({"<http://e/a>", "<http://e/n>", "\"3\"^^<http://www.w3.org/2001/XMLSchema#integer>"}));
  EXPECT_TRUE(rows.count({"<http://e/a>", "<http://e/n>", "\"2.5\"^^<http://www.w3.org/2001/XMLSchema#decimal>"}));
  EXPECT_TRUE(rows.count({"<http://e/a>", "<http://e/n>", "\"true\"^^<http://www.w3.org/2001/XMLSchema#boolean>"}));
}

TEST(TurtleParser, BlankNodesAndCollections) {
  const auto rows = Parse("@prefix ex: <http://e/> .\nex:a ex:p [ ex:q ex:b ] , ( ex:x ex:y ) .\n");
  // a-p-_:b1, _:b1-q-b, a-p-list, and first/rest for two cells (4).
  EXPECT_EQ(rows.size(), 7u);
}

TEST(TurtleParser, EscapesLongStringsAndBase) {
  const auto rows = Parse(
      "@base <http://e/dir/> .\n<x> <#p> \"\"\"two\nlines \"quoted\" \\u00e9\"\"\" .\n");
  ASSERT_EQ(rows.size(), 1u);
  const auto& row = *rows.begin();
  EXPECT_EQ(row[0], "<http://e/dir/x>");
  EXPECT_EQ(row[1], "<http://e/dir/#p>");
  EXPECT_NE(row[2].find("two"), std::string::npos);
}

TEST(TurtleParser, ErrorsNameTheLine) {
  try {
    Parse("@prefix ex: <http://e/> .\nex:a ex:b .\n");
    FAIL() << "expected a parse error";
  } catch (const std::runtime_error& ex) {
    EXPECT_NE(std::string(ex.what()).find("line 2"), std::string::npos) << ex.what();
  }
  EXPECT_THROW(Parse("nope:a <http://p> <http://o> ."), std::runtime_error);
}

// The built-in parser must agree with raptor on real files.
TEST(TurtleParser, AgreesWithRaptorOnTheShippedData) {
  for (const char* name : {"tiny.ttl", "pizza.ttl"}) {
    const auto path = test::DataFile(name);
    MemDictionary dict;
    MemStore store;
    RdfLoader().Load(path, dict, store);
    std::set<Row> expected;
    auto scan = store.Scan(TriplePattern{});
    scan->Init();
    Triple t;
    while (scan->Next(&t)) {
      expected.insert({*dict.Lookup(t.subject), *dict.Lookup(t.predicate), *dict.Lookup(t.object)});
    }
    std::ifstream in(path);
    std::ostringstream text;
    text << in.rdbuf();
    EXPECT_EQ(Parse(text.str()), expected) << name;
  }
}

}  // namespace
}  // namespace ontodb
