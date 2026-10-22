#include <fstream>
#include <sstream>

#include <gtest/gtest.h>

#include "common/database.h"
#include "dictionary/mem_dictionary.h"
#include "semantic/vector_store.h"
#include "shell/shell.h"
#include "test_util.h"

namespace ontodb {
namespace {

constexpr const char* kNs = "http://ex/";

auto Iri(const std::string& name) -> std::string {
  return std::string("<") + kNs + name + ">";
}

// Animal > Mammal > {Dog, Cat}; Animal > Bird > Sparrow. Dog and Cat point the
// same way, Sparrow elsewhere.
struct Fixture {
  test::TempDir dir;
  std::string ontology;
  std::string vectors;

  Fixture() {
    ontology = (dir.path() / "animals.nt").string();
    vectors = (dir.path() / "animals.ovec").string();
    std::ofstream nt(ontology);
    const std::string sub = "<http://www.w3.org/2000/01/rdf-schema#subClassOf>";
    const std::string label = "<http://www.w3.org/2000/01/rdf-schema#label>";
    for (const auto& [c, p] : std::vector<std::pair<std::string, std::string>>{
             {"Mammal", "Animal"}, {"Bird", "Animal"}, {"Dog", "Mammal"}, {"Cat", "Mammal"}, {"Sparrow", "Bird"}}) {
      nt << Iri(c) << " " << sub << " " << Iri(p) << " .\n";
    }
    for (const char* c : {"Animal", "Mammal", "Bird", "Dog", "Cat", "Sparrow"}) {
      nt << Iri(c) << " " << label << " \"" << (std::string(c) == "Dog" ? "dog" : std::string(c)) << "\" .\n";
    }
    nt.close();
    MemDictionary dict;
    VectorStore store(4);
    const std::vector<std::pair<std::string, std::vector<float>>> vecs{
        {"Animal", {1, 1, 0, 0}}, {"Mammal", {1, 0.2f, 0, 0}}, {"Bird", {0, 0, 1, 0.3f}},
        {"Dog", {1, 0.1f, 0, 0}}, {"Cat", {1, 0.15f, 0.05f, 0}}, {"Sparrow", {0, 0.1f, 1, 0.2f}}};
    for (const auto& [name, v] : vecs) {
      store.Put(dict.Insert(Iri(name)), v.data());
    }
    store.Save(vectors, dict);
  }
};

auto RunScript(const std::string& script) -> std::string {
  Database db;
  std::istringstream in(script);
  std::ostringstream out;
  Shell shell(db, in, out);
  shell.Run();
  return out.str();
}

TEST(P14_8_SemanticShell, HierarchyUnderAndSimilar) {
  Fixture f;
  const auto out = RunScript("\\load " + f.ontology + "\n\\hindex build\n\\under <" + std::string(kNs) + "Mammal>\n\\vload " + f.vectors +
                       "\n\\similar <" + std::string(kNs) + "Dog> 2\n\\similar dog 5 under <" + std::string(kNs) + "Bird>\n\\quit\n");
  EXPECT_NE(out.find("labelled 6 classes"), std::string::npos) << out;
  EXPECT_NE(out.find("3 classes at or below"), std::string::npos) << out;
  EXPECT_NE(out.find("<http://ex/Cat>  \"Cat\""), std::string::npos) << out;
  EXPECT_NE(out.find("read 6 vectors of 4 floats, kept 6"), std::string::npos) << out;
  // Dog's nearest neighbor is Cat or Mammal, and Sparrow is not among the two nearest.
  const auto near = out.substr(out.find("nearest to <http://ex/Dog>"));
  const auto first_block = near.substr(0, near.find("nearest to", 5));
  EXPECT_EQ(first_block.find("Sparrow"), std::string::npos) << first_block;
  // Under Bird, by label "dog": only Bird's subtree may come back.
  const auto under = out.substr(out.rfind("nearest to"));
  EXPECT_NE(under.find("Sparrow"), std::string::npos) << under;
  EXPECT_EQ(under.find("Cat"), std::string::npos) << under;
  EXPECT_NE(under.find("under <http://ex/Bird>"), std::string::npos) << under;
}

TEST(P14_8_SemanticShell, IndexesAndEveryStrategyAgreeOnATinyOntology) {
  Fixture f;
  const std::string load = "\\load " + f.ontology + "\n\\hindex build\n\\vload " + f.vectors + "\n\\vindex build all\n";
  for (const char* strategy : {"auto", "pre", "post-hnsw", "post-ivf", "mask-hnsw", "mask-ivf"}) {
    const auto out = RunScript(load + "\\similar <" + std::string(kNs) + "Dog> 3 under <" + std::string(kNs) + "Mammal> using " + strategy + "\n\\quit\n");
    const auto block = out.substr(out.rfind("nearest to"));
    EXPECT_EQ(block.find("Sparrow"), std::string::npos) << strategy << "\n" << block;
    EXPECT_NE(block.find("Cat"), std::string::npos) << strategy << "\n" << block;
  }
  const auto status = RunScript(load + "\\hindex\n\\quit\n");
  EXPECT_NE(status.find("flat, ivf, hnsw"), std::string::npos) << status;
}

TEST(P14_8_SemanticShell, ErrorsAreMessagesNotCrashes) {
  Fixture f;
  const auto out = RunScript("\\similar <http://ex/Dog>\n\\under <http://ex/Mammal>\n\\hindex build\n\\load " + f.ontology +
                       "\n\\hindex build\n\\under <http://ex/Nope>\n\\vload /no/such/file\n\\vload " + f.vectors +
                       "\n\\similar <http://ex/Nope>\n\\similar <http://ex/Dog> under <http://ex/Nope>\n\\similar <http://ex/Dog> using chaos\n"
                       "\\similar <http://ex/Dog> using mask-hnsw\n\\similar <http://ex/Dog> 0\n\\vindex build wat\n\\quit\n");
  EXPECT_NE(out.find("error: no vectors"), std::string::npos) << out;
  EXPECT_NE(out.find("error: no hierarchy"), std::string::npos) << out;
  EXPECT_NE(out.find("is not in the dictionary"), std::string::npos) << out;
  EXPECT_NE(out.find("error: no term named <http://ex/Nope>"), std::string::npos) << out;
  EXPECT_NE(out.find("error: /no/such/file is not a vector file"), std::string::npos) << out;
  EXPECT_NE(out.find("error: unknown strategy chaos"), std::string::npos) << out;
  EXPECT_NE(out.find("that index is not built"), std::string::npos) << out;
  EXPECT_NE(out.find("k must be at least 1"), std::string::npos) << out;
  EXPECT_NE(out.find("usage: \\vindex build"), std::string::npos) << out;
}

TEST(P14_8_SemanticShell, ABackendSwitchDropsTheEngine) {
  Fixture f;
  const auto out = RunScript("\\load " + f.ontology + "\n\\hindex build\n\\set backend indexed\n\\hindex\n\\hindex build\n\\under <http://ex/Mammal>\n\\quit\n");
  EXPECT_NE(out.find("hierarchy: not built"), std::string::npos) << out;
  EXPECT_NE(out.find("3 classes at or below"), std::string::npos) << out;
}

}  // namespace
}  // namespace ontodb
