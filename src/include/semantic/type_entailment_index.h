#pragma once

#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <vector>

#include "buffer/buffer_pool_manager.h"
#include "dictionary/dictionary.h"
#include "execution/type_entailment.h"
#include "semantic/class_hierarchy.h"
#include "semantic/label_index.h"
#include "store/triple_store.h"

namespace ontodb {

// The RDFS subclass entailment of `(x rdf:type C)` as range scans (docs/semantic-search.md,
// section 3): the class hierarchy from the store's rdfs:subClassOf triples, labelled with
// postorder intervals, and a B+ tree of (label of a type, instance) for every rdf:type triple.
// It lives on a private buffer pool over a temporary file, so it works on either backend.
//
// The index is a snapshot. Refresh rebuilds it when the data's version number has moved since the
// last build, which the database bumps on every update, load, and backend switch; a rebuild reads
// the whole hierarchy and every type triple, which takes milliseconds for ontologies of tens of
// thousands of classes. Queries run concurrently with each other; a refresh waits for the ones in
// flight.
class TypeEntailmentIndex : public TypeEntailment {
 public:
  TypeEntailmentIndex();
  ~TypeEntailmentIndex() override;
  TypeEntailmentIndex(const TypeEntailmentIndex&) = delete;
  auto operator=(const TypeEntailmentIndex&) -> TypeEntailmentIndex& = delete;

  // Rebuilds from `store` unless it was last built at `version` from the same store.
  void Refresh(TripleStore* store, Dictionary* dictionary, uint64_t version);

  auto RdfType() const -> term_id_t override;
  auto Covers(term_id_t cls) const -> bool override;
  auto EstimateInstances(term_id_t cls) const -> double override;
  auto Instances(term_id_t cls) const -> std::vector<term_id_t> override;
  auto IsInstance(term_id_t entity, term_id_t cls) const -> bool override;
  // The size in bytes of the label tree's file, which holds one key per rdf:type triple.
  auto IndexBytes() const -> uint64_t;

 private:
  struct Parts;

  mutable std::shared_mutex mu_;
  std::unique_ptr<Parts> parts_;
  TripleStore* store_{nullptr};
  uint64_t version_{0};
  bool built_{false};
  term_id_t type_{INVALID_TERM_ID};
};

}  // namespace ontodb
