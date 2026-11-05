#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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
struct SemanticHit {
  std::string term;  // as the store spells it
  float score{0};    // cosine similarity
};

class SemanticEngine {
 public:
  struct Impl;

  SemanticEngine();
  ~SemanticEngine();
  SemanticEngine(const SemanticEngine&) = delete;
  auto operator=(const SemanticEngine&) -> SemanticEngine& = delete;

  // Labels the hierarchy of `predicate` (default rdfs:subClassOf) from the store.
  auto BuildHierarchy(TripleStore* store, Dictionary* dictionary,
                      const std::string& predicate) -> std::string;
  // The classes at or below `cls`, up to `limit`.
  auto Under(TripleStore* store, Dictionary* dictionary, const std::string& cls,
             size_t limit) -> std::string;
  // Reads a vector file; terms the dictionary lacks are skipped.
  auto LoadVectors(Dictionary* dictionary, const std::string& path) -> std::string;
  // "ivf", "hnsw", or "all".
  auto BuildIndex(const std::string& kind) -> std::string;
  // `<term> [k] [under <class>] [using auto|pre|post-hnsw|post-ivf|mask-hnsw|mask-ivf]`
  auto Similar(TripleStore* store, Dictionary* dictionary, const std::string& args) -> std::string;
  auto Status() const -> std::string;

  // The same operations for programs: results as values, and failures as std::runtime_error.
  // Classes at or below `cls`; a `limit` of 0 means all of them.
  auto UnderTerms(TripleStore* store, Dictionary* dictionary, const std::string& cls,
                  size_t limit) -> std::vector<std::string>;
  // Replaces the vectors with one row of `dim` floats per term in `data`, and drops the approximate
  // indexes. A term the dictionary lacks is skipped. Returns how many were kept.
  auto PutVectors(Dictionary* dictionary, const std::vector<std::string>& terms, const float* data,
                  uint32_t dim) -> size_t;
  // The `k` terms nearest a vector of `dim` floats, which need not be normalized, and which need
  // not be a term's: this is how to search from free text. `under` limits them to a class's
  // subtree.
  auto Nearest(TripleStore* store, Dictionary* dictionary, const float* query, uint32_t dim,
               size_t k, const std::string& under) -> std::vector<SemanticHit>;
  // The `k` terms nearest a term, without the term itself.
  auto NearestTerm(TripleStore* store, Dictionary* dictionary, const std::string& term, size_t k,
                   const std::string& under) -> std::vector<SemanticHit>;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace ontodb
