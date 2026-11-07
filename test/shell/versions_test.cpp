// Named versions through the shell: the commands an ontology's upkeep drives.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "common/database.h"
#include "shell/shell.h"
#include "test_util.h"

namespace ontodb {
namespace {

const std::string kAll = "SELECT ?s ?o WHERE { ?s <http://ex/p> ?o }";

auto Script(Database& db, const std::string& script) -> std::string {
  std::istringstream in(script + "\n\\quit\n");
  std::ostringstream out;
  Shell shell(db, in, out);
  shell.Run();
  return out.str();
}

auto Has(const std::string& text, const std::string& part) -> bool {
  return text.find(part) != std::string::npos;
}

class Versions : public ::testing::Test {
 protected:
  auto File(const std::string& name, const std::string& body) -> std::string {
    const auto path = dir_.path() / name;
    std::ofstream(path) << body;
    return path.string();
  }
  test::TempDir dir_;
};

TEST_F(Versions, TwoVersionsShareWhatTheyHaveInCommonAndReadApart) {
  Database db;
  const auto one = File("one.nt",
                        "<http://ex/a> <http://ex/p> <http://ex/1> .\n"
                        "<http://ex/b> <http://ex/p> <http://ex/2> .\n");
  const auto two = File("two.nt",
                        "<http://ex/b> <http://ex/p> <http://ex/2> .\n"
                        "<http://ex/c> <http://ex/p> <http://ex/3> .\n");
  const auto made = Script(
      db, "\\replace " + one + "\n\\tag 0.1.0\n\\replace " + two + "\n\\tag 0.2.0\n\\versions");
  EXPECT_TRUE(Has(made, "2 added, 0 removed")) << made;
  EXPECT_TRUE(Has(made, "1 added, 1 removed")) << made;
  EXPECT_TRUE(Has(made, "version 0.2.0 is commit 2, 2 triples")) << made;
  EXPECT_TRUE(Has(made, "2 versions; 2 commits, 3 distinct triples in 3 spans")) << made;

  const auto old = Script(db, "\\at 0.1.0\n" + kAll);
  EXPECT_TRUE(Has(old, "reading version 0.1.0, commit 1, 2 triples")) << old;
  EXPECT_TRUE(Has(old, "<http://ex/a>")) << old;
  EXPECT_FALSE(Has(old, "<http://ex/c>")) << old;
  const auto latest = Script(db, "\\at latest\n" + kAll);
  EXPECT_TRUE(Has(latest, "<http://ex/c>")) << latest;
  EXPECT_FALSE(Has(latest, "<http://ex/a>")) << latest;
}

TEST_F(Versions, ReplacingWithTheSameFileIsNoCommit) {
  Database db;
  const auto one = File("one.nt", "<http://ex/a> <http://ex/p> <http://ex/1> .\n");
  const auto out = Script(db, "\\replace " + one + "\n\\replace " + one);
  EXPECT_TRUE(Has(out, "0 added, 0 removed")) << out;
  EXPECT_EQ(db.GetVersions().Clock(), 1U);
}

TEST_F(Versions, AVersionCannotBeWrittenAndItsNameCannotMove) {
  Database db;
  const auto one = File("one.nt", "<http://ex/a> <http://ex/p> <http://ex/1> .\n");
  const auto two = File("two.nt", "<http://ex/b> <http://ex/p> <http://ex/2> .\n");
  const auto out =
      Script(db, "\\replace " + one + "\n\\tag 1\n\\at 1\n" +
                     "INSERT DATA { <http://ex/z> <http://ex/p> <http://ex/9> . }\n" + "\\load " +
                     two + "\n\\replace " + two + "\n\\begin\n\\at latest\n" + "\\replace " + two +
                     "\n\\tag 1\n\\at nothing");
  EXPECT_TRUE(Has(out, "an update cannot run at a version")) << out;
  EXPECT_TRUE(Has(out, "\\load cannot run at a version")) << out;
  EXPECT_TRUE(Has(out, "\\replace cannot run at a version")) << out;
  EXPECT_TRUE(Has(out, "\\begin cannot run at a version")) << out;
  EXPECT_TRUE(Has(out, "already names timestamp 1")) << out;
  EXPECT_TRUE(Has(out, "no version named nothing")) << out;
}

TEST_F(Versions, AMissingFileChangesNothing) {
  Database db;
  const auto one = File("one.nt", "<http://ex/a> <http://ex/p> <http://ex/1> .\n");
  const auto out = Script(db, "\\replace " + one + "\n\\replace /no/such.nt\n" + kAll);
  EXPECT_TRUE(Has(out, "error:")) << out;
  EXPECT_TRUE(Has(out, "<http://ex/a>")) << out;
  EXPECT_EQ(db.GetVersions().Clock(), 1U);
}

TEST_F(Versions, EachSessionReadsItsOwnVersionWhileAnotherWrites) {
  Database db;
  const auto one = File("one.nt", "<http://ex/a> <http://ex/p> <http://ex/1> .\n");
  const auto out = Script(db, "\\replace " + one +
                                  "\n\\tag 1\n\\session 2\n\\at 1\n\\session 1\n"
                                  "DELETE DATA { <http://ex/a> <http://ex/p> <http://ex/1> . }\n" +
                                  kAll + "\n\\session 2\n" + kAll);
  const auto last = out.rfind("<http://ex/a>");
  ASSERT_NE(last, std::string::npos) << out;
  EXPECT_NE(out.find("(0 rows)"), std::string::npos) << out;  // session 1, at the latest
  EXPECT_GT(last, out.find("(0 rows)")) << out;               // session 2, at the version
}

TEST_F(Versions, TheHierarchyIsTheVersionsAndSaysWhenItIsNot) {
  Database db;
  const std::string sub = " <http://www.w3.org/2000/01/rdf-schema#subClassOf> ";
  const auto one = File("one.nt", "<http://ex/Dog>" + sub + "<http://ex/Animal> .\n");
  const auto two = File("two.nt", "<http://ex/Cat>" + sub + "<http://ex/Animal> .\n");
  const auto out =
      Script(db, "\\replace " + one + "\n\\tag 1\n\\replace " + two +
                     "\n\\at 1\n\\hindex build\n\\under <http://ex/Animal>\n\\at latest\n"
                     "\\under <http://ex/Animal>\n\\hindex build\n"
                     "\\under <http://ex/Animal>");
  const auto dog = out.find("<http://ex/Dog>");
  const auto refused = out.find("built at another version");
  const auto cat = out.find("<http://ex/Cat>");
  ASSERT_NE(dog, std::string::npos) << out;
  ASSERT_NE(refused, std::string::npos) << out;
  ASSERT_NE(cat, std::string::npos) << out;
  EXPECT_LT(dog, refused);
  EXPECT_LT(refused, cat);
}

TEST_F(Versions, EverythingSurvivesAReopenInTheShell) {
  const auto one = File("one.nt",
                        "<http://ex/a> <http://ex/p> <http://ex/1> .\n"
                        "<http://ex/b> <http://ex/p> <http://ex/2> .\n");
  const auto two = File("two.nt", "<http://ex/b> <http://ex/p> <http://ex/2> .\n");
  const auto dir = dir_.path() / "db";
  {
    auto db = Database::Open(dir);
    Script(*db,
           "\\replace " + one + "\n\\tag 0.1.0\n\\checkpoint\n\\replace " + two + "\n\\tag 0.2.0");
  }
  auto db = Database::Open(dir);
  const auto out = Script(*db, "\\verify\n\\at 0.1.0\n" + kAll + "\n\\at 0.2.0\n" + kAll);
  EXPECT_TRUE(Has(out, "verify: ok")) << out;
  EXPECT_TRUE(Has(out, "<http://ex/a>")) << out;
  EXPECT_TRUE(Has(out, "(2 rows)")) << out;
  EXPECT_TRUE(Has(out, "(1 row)")) << out;
}

TEST_F(Versions, ARealOntologyComesBackExactlyAtEachVersion) {
  const auto path = test::DataFile("tiny.owl").string();
  Database db;
  const auto base = Script(db, "\\load " + path + "\n\\tag base");
  EXPECT_TRUE(Has(base, "version base is commit 1")) << base;
  const auto count = db.GetVersions().Count(1);
  EXPECT_GT(count, 0U);
  const auto emptied = File("nothing.nt", "<http://ex/x> <http://ex/p> <http://ex/y> .\n");
  Script(db, "\\replace " + emptied + "\n\\tag other");
  EXPECT_EQ(db.GetVersions().Count(2), 1U);
  const auto back = Script(db, "\\replace " + path + "\n\\tag again");
  EXPECT_TRUE(Has(back, "version again is commit 3")) << back;
  EXPECT_EQ(db.GetVersions().Count(3), count);
  const auto first = db.GetVersions().Scan({}, 1);
  auto third = db.GetVersions().Scan({}, 3);
  EXPECT_EQ(first, third);
}

}  // namespace
}  // namespace ontodb
