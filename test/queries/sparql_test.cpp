#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "common/database.h"

namespace ontodb {
namespace {

auto Rows(Database& db, const std::string& query) -> std::vector<std::string> {
  auto result = db.Execute(query, nullptr);
  std::vector<std::string> lines;
  for (const auto& row : result.rows) {
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

TEST(Sparql, OptionalUnionMinusBindValuesAndPaths) {
  Database db;
  db.Execute(
      "INSERT DATA { <http://ex/a> <http://ex/p> <http://ex/b> . "
      "<http://ex/b> <http://ex/p> <http://ex/c> . "
      "<http://ex/a> <http://ex/q> <http://ex/d> . }",
      nullptr);
  EXPECT_EQ(Rows(db, "SELECT ?a ?c WHERE { ?a <http://ex/p> ?b OPTIONAL { ?b <http://ex/q> ?c } }"),
            (std::vector<std::string>{"<http://ex/a>\t", "<http://ex/b>\t"}));
  // UNION keeps both sides, so the same binding from each side appears twice.
  EXPECT_EQ(Rows(db,
                 "SELECT ?x WHERE { { ?x <http://ex/p> <http://ex/b> } UNION { ?x <http://ex/q> "
                 "<http://ex/d> } }"),
            (std::vector<std::string>{"<http://ex/a>", "<http://ex/a>"}));
  EXPECT_EQ(Rows(db, "SELECT ?a WHERE { ?a <http://ex/p> ?b MINUS { ?a <http://ex/q> ?c } }"),
            (std::vector<std::string>{"<http://ex/b>"}));
  EXPECT_EQ(
      Rows(db, "SELECT ?a ?c WHERE { ?a <http://ex/p> ?b BIND(?b AS ?c) }"),
      (std::vector<std::string>{"<http://ex/a>\t<http://ex/b>", "<http://ex/b>\t<http://ex/c>"}));
  EXPECT_EQ(
      Rows(db, "SELECT ?a WHERE { VALUES ?a { <http://ex/a> <http://ex/z> } ?a <http://ex/p> ?b }"),
      (std::vector<std::string>{"<http://ex/a>"}));
  EXPECT_EQ(Rows(db, "SELECT ?c WHERE { <http://ex/a> <http://ex/p>+ ?c }"),
            (std::vector<std::string>{"<http://ex/b>", "<http://ex/c>"}));
  EXPECT_EQ(Rows(db, "SELECT ?c WHERE { <http://ex/a> <http://ex/p>* ?c }").size(), 3u);
  const auto nested = Rows(db, "SELECT ?b WHERE { { <http://ex/a> <http://ex/p> ?b } }");
  EXPECT_EQ(nested, (std::vector<std::string>{"<http://ex/b>"}));
}

TEST(Sparql, AggregatesGraphServiceAndUpdateWhere) {
  Database db;
  db.Execute(
      "INSERT DATA { <http://ex/a> <http://ex/p> \"1\"^^<http://www.w3.org/2001/XMLSchema#integer> "
      ". "
      "<http://ex/a> <http://ex/p> \"3\"^^<http://www.w3.org/2001/XMLSchema#integer> . "
      "<http://ex/b> <http://ex/p> \"2\"^^<http://www.w3.org/2001/XMLSchema#integer> . }",
      nullptr);
  EXPECT_EQ(Rows(db, "SELECT (COUNT(?o) AS ?c) WHERE { ?s <http://ex/p> ?o }"),
            (std::vector<std::string>{"\"3\"^^<http://www.w3.org/2001/XMLSchema#integer>"}));
  EXPECT_EQ(Rows(db, "SELECT ?s (COUNT(?o) AS ?c) WHERE { ?s <http://ex/p> ?o } GROUP BY ?s"),
            (std::vector<std::string>{
                "<http://ex/a>\t\"2\"^^<http://www.w3.org/2001/XMLSchema#integer>",
                "<http://ex/b>\t\"1\"^^<http://www.w3.org/2001/XMLSchema#integer>"}));
  EXPECT_EQ(Rows(db, "SELECT ?s WHERE { GRAPH <urn:ontodb:graph> { ?s <http://ex/p> ?o } }").size(),
            3u);
  EXPECT_EQ(
      Rows(db, "SELECT ?s WHERE { SERVICE <http://ex/sparql> { ?s <http://ex/p> ?o } }").size(),
      3u);
  db.Execute(
      "DELETE { ?s <http://ex/p> ?o } INSERT { ?s <http://ex/q> ?o } WHERE { ?s <http://ex/p> ?o }",
      nullptr);
  EXPECT_TRUE(Rows(db, "SELECT ?s WHERE { ?s <http://ex/p> ?o }").empty());
  EXPECT_EQ(Rows(db, "SELECT ?s WHERE { ?s <http://ex/q> ?o }").size(), 3u);
}

TEST(Rdfs, MaterializesSubclassAndDropsItWhenThePremiseGoes) {
  Database db;
  db.SetInference(true);
  db.Execute(
      "INSERT DATA { <http://ex/Grad> <http://www.w3.org/2000/01/rdf-schema#subClassOf> "
      "<http://ex/Student> . "
      "<http://ex/Student> <http://www.w3.org/2000/01/rdf-schema#subClassOf> <http://ex/Person> . "
      "<http://ex/ann> <http://www.w3.org/1999/02/22-rdf-syntax-ns#type> <http://ex/Grad> . "
      "<http://ex/name> <http://www.w3.org/2000/01/rdf-schema#subPropertyOf> <http://ex/label> . "
      "<http://ex/name> <http://www.w3.org/2000/01/rdf-schema#domain> <http://ex/Person> . "
      "<http://ex/ann> <http://ex/name> \"Ann\" . }",
      nullptr);
  EXPECT_EQ(Rows(db,
                 "SELECT ?t WHERE { <http://ex/ann> "
                 "<http://www.w3.org/1999/02/22-rdf-syntax-ns#type> ?t }")
                .size(),
            3u);
  EXPECT_EQ(Rows(db, "SELECT ?o WHERE { <http://ex/ann> <http://ex/label> ?o }").size(), 1u);
  db.Execute(
      "DELETE DATA { <http://ex/ann> <http://www.w3.org/1999/02/22-rdf-syntax-ns#type> "
      "<http://ex/Grad> }",
      nullptr);
  // The stated type is gone, and so are the subclass inferences. domain still
  // types Ann as a Person because the name triple remains.
  EXPECT_EQ(Rows(db,
                 "SELECT ?t WHERE { <http://ex/ann> "
                 "<http://www.w3.org/1999/02/22-rdf-syntax-ns#type> ?t }"),
            (std::vector<std::string>{"<http://ex/Person>"}));
  db.SetInference(false);
  EXPECT_TRUE(Rows(db, "SELECT ?o WHERE { <http://ex/ann> <http://ex/label> ?o }").empty());
  EXPECT_EQ(Rows(db, "SELECT ?o WHERE { <http://ex/ann> <http://ex/name> ?o }").size(), 1u);
}

TEST(Sparql, BlankNodesAndCollections) {
  Database db;
  db.Execute("INSERT DATA { [ <http://ex/p> <http://ex/b> ] <http://ex/r> <http://ex/a> }",
             nullptr);
  EXPECT_EQ(Rows(db, "SELECT ?b WHERE { ?s <http://ex/p> ?b . ?s <http://ex/r> <http://ex/a> }"),
            (std::vector<std::string>{"<http://ex/b>"}));
  db.Execute("INSERT DATA { <http://ex/s> <http://ex/list> ( <http://ex/a> <http://ex/b> ) }",
             nullptr);
  EXPECT_EQ(Rows(db,
                 "SELECT ?a WHERE { <http://ex/s> <http://ex/list> ?h . ?h "
                 "<http://www.w3.org/1999/02/22-rdf-syntax-ns#first> ?a }"),
            (std::vector<std::string>{"<http://ex/a>"}));
}

}  // namespace
}  // namespace ontodb
