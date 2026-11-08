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

TEST(Parser, AcceptsTheFormsThatUsedToBeRejected) {
  EXPECT_EQ(Parser("SELECT ?a WHERE { ?a <p> ?b OPTIONAL { ?a <q> ?c } }").Parse().pattern->kind,
            AstPattern::Kind::kJoin);
  EXPECT_EQ(Parser("SELECT ?a WHERE { { ?a <p> ?b } UNION { ?a <q> ?c } }").Parse().pattern->kind,
            AstPattern::Kind::kUnion);
  EXPECT_NE(Parser("SELECT ?a WHERE { ?a <p>/<q> ?b }").Parse().pattern, nullptr);
  const auto agg = Parser("SELECT (COUNT(?a) AS ?c) WHERE { ?a <p> ?b }").Parse();
  ASSERT_EQ(agg.aggregates.size(), 1u);
  EXPECT_EQ(agg.aggregates[0].alias, "c");
  const auto reduced = Parser("SELECT REDUCED ?a WHERE { ?a <p> ?b }").Parse();
  EXPECT_TRUE(reduced.reduced);
  const auto update =
      Parser("DELETE { ?a <p> ?b } INSERT { ?a <q> ?b } WHERE { ?a <p> ?b }").Parse();
  EXPECT_TRUE(update.update_where);
  EXPECT_EQ(update.delete_template.size(), 1u);
  EXPECT_EQ(update.insert_template.size(), 1u);
  EXPECT_NE(Parser("SELECT ?a WHERE { ?a <p> ?b MINUS { ?a <q> ?c } }").Parse().pattern, nullptr);
  EXPECT_NE(Parser("SELECT ?a WHERE { ?a <p> ?b . BIND(?a AS ?c) }").Parse().pattern, nullptr);
  EXPECT_NE(Parser("SELECT ?a WHERE { VALUES ?a { <http://ex/a> } }").Parse().pattern, nullptr);
  EXPECT_NE(Parser("SELECT ?a WHERE { GRAPH <urn:ontodb:graph> { ?a <p> ?b } }").Parse().pattern,
            nullptr);
  EXPECT_NE(
      Parser("SELECT ?a WHERE { SERVICE SILENT <http://ex/sparql> { ?a <p> ?b } }").Parse().pattern,
      nullptr);
  EXPECT_NE(Parser("SELECT ?a WHERE { [ <p> ?a ] <q> <o> }").Parse().triples.size(), 0u);
  EXPECT_EQ(
      Parser("SELECT ?a WHERE { ?a <p> ( <http://ex/a> <http://ex/b> ) }").Parse().triples.size(),
      5u);
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
