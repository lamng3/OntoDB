#pragma once

#include <vector>

#include "common/types.h"

namespace ontodb {

// What the planner and the scan need to answer `(x rdf:type C)` under RDFS subclass entailment
// without storing the entailed triples: the instances of C are the subjects that have a type at or
// below C. An implementation labels the class hierarchy with postorder intervals and keeps the
// instances in a B+ tree keyed by the label of their type (semantic/type_entailment_index.h), so
// the instances are a few range scans. All methods are safe to call from several threads.
class TypeEntailment {
 public:
  virtual ~TypeEntailment() = default;

  // The id of rdf:type, or INVALID_TERM_ID when the store has no such term.
  virtual auto RdfType() const -> term_id_t = 0;
  // Whether `cls` is a class of the hierarchy. A pattern on any other class is an ordinary scan.
  virtual auto Covers(term_id_t cls) const -> bool = 0;
  // An upper bound on how many instances `cls` has, for the optimizer, without scanning them.
  virtual auto EstimateInstances(term_id_t cls) const -> double = 0;
  // The distinct instances of `cls`, ascending.
  virtual auto Instances(term_id_t cls) const -> std::vector<term_id_t> = 0;
  // Whether `entity` has a type at or below `cls`.
  virtual auto IsInstance(term_id_t entity, term_id_t cls) const -> bool = 0;
};

}  // namespace ontodb
