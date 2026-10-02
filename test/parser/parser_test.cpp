#include "parser/parser.h"

#include <gtest/gtest.h>

#include "binder/binder.h"
#include "common/exception.h"
#include "dictionary/mem_dictionary.h"

namespace ontodb {
namespace {

TEST(Parser, SelectWithAbbreviationAndFilter) {
  const char* query = R"(
    PREFIX ub: <http://example.edu/univ#>
    PREFIX xsd: <http://www.w3.org/2001/XMLSchema#>
    SELECT ?name WHERE {
      ?s a ub:Student ;
         ub:name ?name ;
         ub:age ?age .
      FILTER (?age > "21"^^xsd:integer && ?name != "Cara")
    }
  )";
  const auto ast = Parser(query).Parse();
  EXPECT_EQ(ast.kind, AstQuery::Kind::kSelect);
  EXPECT_EQ(ast.triples.size(), 3);
  EXPECT_EQ(ast.triples[0].predicate.kind, AstTerm::Kind::kRdfType);
  EXPECT_EQ(ast.select_vars, std::vector<std::string>{"name"});
  ASSERT_NE(ast.filter, nullptr);
  EXPECT_EQ(ast.filter->op, AstFilter::Op::kAnd);
}

TEST(Parser, BareIriKeepsDots) {
  const auto ast = Parser(
                       "SELECT ?x WHERE { ?x <http://ex/p> "
                       "http://www.Department0.University0.edu/GraduateCourse0 }")
                       .Parse();
  ASSERT_EQ(ast.triples.size(), 1);
  EXPECT_EQ(ast.triples[0].object.kind, AstTerm::Kind::kIri);
  EXPECT_EQ(ast.triples[0].object.value, "http://www.Department0.University0.edu/GraduateCourse0");
}

TEST(Parser, InsertAndDeleteData) {
  auto insert = Parser("INSERT DATA { <a> <b> <c> . }").Parse();
  EXPECT_EQ(insert.kind, AstQuery::Kind::kInsert);
  EXPECT_EQ(insert.triples.size(), 1);
  auto del = Parser("DELETE DATA { <a> <b> <c> }").Parse();
  EXPECT_EQ(del.kind, AstQuery::Kind::kDelete);
}

TEST(Parser, BindsOrderLimitDistinctWithoutExecuting) {
  const auto ast =
      Parser("SELECT DISTINCT ?a WHERE { ?a <p> ?b } ORDER BY DESC(?b) LIMIT 2 OFFSET 1").Parse();
  EXPECT_TRUE(ast.distinct);
  ASSERT_EQ(ast.order_by.size(), 1);
  EXPECT_FALSE(ast.order_by[0].ascending);
  EXPECT_EQ(ast.limit, 2);
  EXPECT_EQ(ast.offset, 1);
}

TEST(Parser, UnsupportedFeaturesNameThePlace) {
  auto fails = [](const char* query, const char* needle) {
    try {
      Parser(query).Parse();
      FAIL() << query;
    } catch (const ParseException& ex) {
      EXPECT_NE(std::string(ex.what()).find(needle), std::string::npos) << ex.what();
      EXPECT_GE(ex.line(), 1);
      EXPECT_GE(ex.column(), 1);
    }
  };
  fails("SELECT ?a WHERE { ?a <p> ?b OPTIONAL { ?a <q> ?c } }", "OPTIONAL");
  fails("SELECT ?a WHERE { ?a <p> ?b UNION ?a <q> ?c }", "UNION");
  fails("SELECT ?a WHERE { ?a <p>/<q> ?b }", "property paths");
  fails("SELECT (COUNT(?a) AS ?c) WHERE { ?a <p> ?b }", "aggregates");
  fails("DELETE { ?a <p> ?b } WHERE { ?a <p> ?b }", "DELETE/INSERT WHERE");
  fails("INSERT { ?a <p> ?b } WHERE { ?a <p> ?b }", "DELETE/INSERT WHERE");
}

TEST(Binder, ResolvesPrefixAndRejectsUnknown) {
  MemDictionary dict;
  const auto ast = Parser("PREFIX ub: <http://ex/> SELECT ?s WHERE { ?s ub:name \"A\" }").Parse();
  const auto bound = Binder(dict).Bind(ast);
  EXPECT_EQ(bound.var_names.size(), 1);
  EXPECT_EQ(bound.patterns[0].predicate.text, "<http://ex/name>");
  EXPECT_THROW(Binder(dict).Bind(Parser("SELECT ?s WHERE { ?s missing:name \"A\" }").Parse()),
               BindException);
  EXPECT_THROW(Binder(dict).Bind(Parser("INSERT DATA { ?s <p> <o> }").Parse()), BindException);
}

}  // namespace
}  // namespace ontodb
