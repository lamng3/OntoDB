#include <gtest/gtest.h>

#include "common/database.h"
#include "common/exception.h"

namespace ontodb {
namespace {

auto Exec(Database& db, const std::string& query) -> StatementResult {
  return db.Execute(query, nullptr);
}

TEST(Execution, JoinFilterAndProjection) {
  Database db;
  EXPECT_NE(
      Exec(db,
           "INSERT DATA { <http://ex/a> <http://ex/type> <http://ex/Student> . "
           "<http://ex/a> <http://ex/name> \"Alice\" . "
           "<http://ex/a> <http://ex/age> \"20\"^^<http://www.w3.org/2001/XMLSchema#integer> . "
           "<http://ex/b> <http://ex/type> <http://ex/Student> . "
           "<http://ex/b> <http://ex/name> \"Bob\" . "
           "<http://ex/b> <http://ex/age> \"22\"^^<http://www.w3.org/2001/XMLSchema#integer> . }")
          .message.find("inserted 6"),
      std::string::npos);

  auto older = Exec(db,
                    "SELECT ?name WHERE { "
                    "?s <http://ex/type> <http://ex/Student> . "
                    "?s <http://ex/name> ?name . "
                    "?s <http://ex/age> ?age . "
                    "FILTER (?age > \"21\"^^<http://www.w3.org/2001/XMLSchema#integer>) }");
  ASSERT_EQ(older.rows.size(), 1);
  EXPECT_EQ(older.rows[0][0], "\"Bob\"");

  auto plan = db.Explain(
      "SELECT ?name WHERE { ?s <http://ex/name> ?name . ?s <http://ex/type> <http://ex/Student> }");
  EXPECT_NE(plan.find("NestedLoopJoin"), std::string::npos);
  EXPECT_NE(plan.find("Projection"), std::string::npos);
}

TEST(Execution, SameVariableInOnePattern) {
  Database db;
  Exec(db,
       "INSERT DATA { <http://ex/a> <http://ex/p> <http://ex/a> . <http://ex/a> <http://ex/p> "
       "<http://ex/b> }");
  auto result = Exec(db, "SELECT ?s WHERE { ?s <http://ex/p> ?s }");
  ASSERT_EQ(result.rows.size(), 1);
  EXPECT_EQ(result.rows[0][0], "<http://ex/a>");
}

TEST(Execution, DeleteIsSilentWhenMissing) {
  Database db;
  auto deleted = Exec(db, "DELETE DATA { <http://ex/a> <http://ex/p> <http://ex/b> }");
  EXPECT_NE(deleted.message.find("deleted 0"), std::string::npos);
}

TEST(Execution, OrderByIsBoundButNotRun) {
  Database db;
  EXPECT_THROW(Exec(db, "SELECT ?s WHERE { ?s <http://ex/p> <http://ex/o> } ORDER BY ?s"),
               NotImplementedException);
}

TEST(ExplainSmoke, ShowsJoinAndFilter) {
  Database db;
  const auto plan = db.Explain(
      "SELECT ?n WHERE { ?s <http://ex/name> ?n . ?s <http://ex/age> ?a . FILTER (?a > "
      "\"1\"^^<http://www.w3.org/2001/XMLSchema#integer>) }");
  EXPECT_NE(plan.find("TripleScan"), std::string::npos);
  EXPECT_NE(plan.find("Filter"), std::string::npos);
}

}  // namespace
}  // namespace ontodb
