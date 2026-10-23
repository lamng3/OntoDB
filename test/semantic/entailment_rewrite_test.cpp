#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>

#include "common/database.h"
#include "test_util.h"

namespace ontodb {
namespace {

// The RDFS subclass entailment of `(x rdf:type C)`, answered by interval scans (\set entail on),
// must return exactly what a store holding the entailed type triples returns. The oracle is a
// second database with the closure written out, queried with entailment off.

constexpr char kType[] = "<http://www.w3.org/1999/02/22-rdf-syntax-ns#type>";
constexpr char kSub[] = "<http://www.w3.org/2000/01/rdf-schema#subClassOf>";

auto Cls(int i) -> std::string { return "<http://ex/C" + std::to_string(i) + ">"; }
auto Ent(int i) -> std::string { return "<http://ex/e" + std::to_string(i) + ">"; }

struct Ontology {
  int classes{0};
  std::set<std::pair<int, int>> edges;                // (class, parent)
  std::map<int, std::set<int>> types;                 // entity -> direct types
  std::map<int, std::vector<int>> knows;              // entity -> friends

  auto Ancestors(int c) const -> std::set<int> {
    std::set<int> seen{c};
    std::vector<int> stack{c};
    while (!stack.empty()) {
      const int x = stack.back();
      stack.pop_back();
      for (const auto& [child, parent] : edges) {
        if (child == x && seen.insert(parent).second) {
          stack.push_back(parent);
        }
      }
    }
    return seen;
  }

  auto Text(bool closure) const -> std::string {
    std::ostringstream out;
    for (const auto& [c, p] : edges) {
      out << Cls(c) << ' ' << kSub << ' ' << Cls(p) << " .\n";
    }
    for (const auto& [e, direct] : types) {
      std::set<int> all = direct;
      if (closure) {
        for (const int t : direct) {
          for (const int a : Ancestors(t)) {
            all.insert(a);
          }
        }
      }
      for (const int t : all) {
        out << Ent(e) << ' ' << kType << ' ' << Cls(t) << " .\n";
      }
    }
    for (const auto& [e, friends] : knows) {
      for (const int f : friends) {
        out << Ent(e) << " <http://ex/knows> " << Ent(f) << " .\n";
      }
    }
    return out.str();
  }
};

auto RandomOntology(unsigned seed) -> Ontology {
  std::mt19937 rng(seed);
  Ontology o;
  o.classes = 12 + static_cast<int>(rng() % 20);
  for (int c = 1; c < o.classes; c++) {
    const int parents = 1 + static_cast<int>(rng() % 2);
    for (int k = 0; k < parents; k++) {
      o.edges.insert({c, static_cast<int>(rng() % static_cast<unsigned>(c))});  // parents precede: a DAG
    }
  }
  if (seed % 3 == 0) {
    o.edges.insert({0, o.classes - 1});  // a cycle: the classes on it are equivalent
  }
  const int entities = 60 + static_cast<int>(rng() % 60);
  for (int e = 0; e < entities; e++) {
    const int n = 1 + static_cast<int>(rng() % 3);
    for (int k = 0; k < n; k++) {
      o.types[e].insert(static_cast<int>(rng() % static_cast<unsigned>(o.classes)));
    }
    o.knows[e].push_back(static_cast<int>(rng() % static_cast<unsigned>(entities)));
  }
  return o;
}

void Fill(Database& db, const Ontology& o, bool closure, const std::filesystem::path& file) {
  {
    std::ofstream out(file);
    out << o.Text(closure);
  }
  db.Load(file.string());
}

auto Rows(Database& db, const std::string& query) -> std::vector<std::string> {
  const auto result = db.Execute(query, nullptr);
  std::vector<std::string> rows;
  for (const auto& row : result.rows) {
    std::string line;
    for (const auto& cell : row) {
      line += cell + "|";
    }
    rows.push_back(line);
  }
  std::sort(rows.begin(), rows.end());
  return rows;
}

void CompareAll(Database& entail, Database& oracle, const Ontology& o, unsigned seed) {
  std::mt19937 rng(seed * 7919U);
  std::vector<std::string> queries;
  for (int i = 0; i < 12; i++) {
    const int c = static_cast<int>(rng() % static_cast<unsigned>(o.classes));
    const int d = static_cast<int>(rng() % static_cast<unsigned>(o.classes));
    const int e = static_cast<int>(rng() % static_cast<unsigned>(o.types.size()));
    queries.push_back("SELECT ?x WHERE { ?x a " + Cls(c) + " }");
    queries.push_back("SELECT ?x ?y WHERE { ?x a " + Cls(c) + " . ?x <http://ex/knows> ?y }");
    queries.push_back("SELECT ?x ?y WHERE { ?x a " + Cls(c) + " . ?x <http://ex/knows> ?y . ?y a " + Cls(d) + " }");
    queries.push_back("SELECT ?x WHERE { ?x a " + Cls(c) + " . ?x a " + Cls(d) + " }");
    queries.push_back("SELECT ?y WHERE { " + Ent(e) + " a " + Cls(c) + " . " + Ent(e) + " <http://ex/knows> ?y }");
    queries.push_back("SELECT DISTINCT ?y WHERE { ?x <http://ex/knows> ?y . ?y a " + Cls(c) + " }");
  }
  queries.push_back("SELECT ?x WHERE { ?x a <http://ex/NoSuchClass> }");
  for (const auto& q : queries) {
    EXPECT_EQ(Rows(entail, q), Rows(oracle, q)) << "seed " << seed << ": " << q;
  }
}

TEST(EntailmentRewrite, MatchesAMaterializedClosureOnBothBackends) {
  test::TempDir dir;
  for (unsigned seed = 1; seed <= 9; seed++) {
    const Ontology o = RandomOntology(seed);
    for (const BackendKind backend : {BackendKind::kMem, BackendKind::kIndexed}) {
      Database entail;
      Database oracle;
      entail.SetBackend(backend);
      oracle.SetBackend(backend);
      Fill(entail, o, false, dir.path() / "base.nt");
      Fill(oracle, o, true, dir.path() / "closed.nt");
      Config config = entail.GetConfig();
      config.entail = true;
      entail.UpdateConfig(config);
      CompareAll(entail, oracle, o, seed);
    }
  }
}

TEST(EntailmentRewrite, FollowsUpdatesToInstancesAndToTheHierarchy) {
  test::TempDir dir;
  Ontology o = RandomOntology(4);
  Database entail;
  Fill(entail, o, false, dir.path() / "base.nt");
  Config config = entail.GetConfig();
  config.entail = true;
  entail.UpdateConfig(config);
  const std::string everything = "SELECT ?x WHERE { ?x a " + Cls(0) + " }";
  Rows(entail, everything);  // builds the index
  // A new instance of a leaf class, then a new subclass edge that moves a class under C0.
  entail.Execute("INSERT DATA { <http://ex/e9999> " + std::string(kType) + " " + Cls(o.classes - 2) + " }", nullptr);
  o.types[9999].insert(o.classes - 2);
  entail.Execute("INSERT DATA { " + Cls(o.classes - 2) + " " + kSub + " " + Cls(0) + " }", nullptr);
  o.edges.insert({o.classes - 2, 0});
  Database oracle;
  Fill(oracle, o, true, dir.path() / "closed.nt");
  CompareAll(entail, oracle, o, 4);
  const auto rows = Rows(entail, everything);
  EXPECT_TRUE(std::find(rows.begin(), rows.end(), "<http://ex/e9999>|") != rows.end());
}

// The rewrite answers a pattern whose class is a constant. A pattern with a class variable is an
// ordinary scan and returns the stored (direct) types, with entailment on or off.
TEST(EntailmentRewrite, AVariableClassStaysAPlainScan) {
  test::TempDir dir;
  const Ontology o = RandomOntology(5);
  Database off;
  Database on;
  Fill(off, o, false, dir.path() / "a.nt");
  Fill(on, o, false, dir.path() / "b.nt");
  Config config = on.GetConfig();
  config.entail = true;
  on.UpdateConfig(config);
  const std::string q = "SELECT ?x ?c WHERE { ?x a ?c }";
  EXPECT_EQ(Rows(on, q), Rows(off, q));
}

TEST(EntailmentRewrite, ExplainShowsTheIntervalScanOnlyWhenOn) {
  test::TempDir dir;
  const Ontology o = RandomOntology(2);
  Database db;
  Fill(db, o, false, dir.path() / "base.nt");
  const std::string q = "SELECT ?x WHERE { ?x a " + Cls(0) + " }";
  EXPECT_EQ(db.Explain(q).find("IntervalTypeScan"), std::string::npos);
  Config config = db.GetConfig();
  config.entail = true;
  db.UpdateConfig(config);
  EXPECT_NE(db.Explain(q).find("IntervalTypeScan"), std::string::npos);
}

}  // namespace
}  // namespace ontodb
