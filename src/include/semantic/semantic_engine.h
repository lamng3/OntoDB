#pragma once

#include <memory>
#include <string>

#include "dictionary/dictionary.h"
#include "store/triple_store.h"

namespace ontodb {

// What the shell's semantic commands drive: a class hierarchy labelled from
// the store's rdfs:subClassOf triples, one vector per term, and the indexes
// over them (docs/semantic-search.md). It owns a private buffer pool on a
// temporary file for the label tree and the IVF lists, so it works on either
// backend. Every method returns the text the shell prints; failures are
// reported as "error: ..." lines, not exceptions.
//
// Terms are written as the store spells them, `<iri>`, or by label: a quoted
// string or bare words name the term whose rdfs:label is that text. Ids are
// the dictionary's, so the engine must be dropped when the backend changes.
//
// Not thread-safe; the shell calls it from one session at a time.
class SemanticEngine {
 public:
  SemanticEngine();
  ~SemanticEngine();
  SemanticEngine(const SemanticEngine&) = delete;
  auto operator=(const SemanticEngine&) -> SemanticEngine& = delete;

  // Labels the hierarchy of `predicate` (default rdfs:subClassOf) from the store.
  auto BuildHierarchy(TripleStore* store, Dictionary* dictionary, const std::string& predicate) -> std::string;
  // The classes at or below `cls`, up to `limit`.
  auto Under(TripleStore* store, Dictionary* dictionary, const std::string& cls, size_t limit) -> std::string;
  // Reads a vector file; terms the dictionary lacks are skipped.
  auto LoadVectors(Dictionary* dictionary, const std::string& path) -> std::string;
  // "ivf", "hnsw", or "all".
  auto BuildIndex(const std::string& kind) -> std::string;
  // `<term> [k] [under <class>] [using auto|pre|post-hnsw|post-ivf|mask-hnsw|mask-ivf]`
  auto Similar(TripleStore* store, Dictionary* dictionary, const std::string& args) -> std::string;
  auto Status() const -> std::string;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ontodb
