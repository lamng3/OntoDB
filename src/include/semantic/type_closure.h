#pragma once

#include <utility>
#include <vector>

#include "common/types.h"
#include "semantic/class_hierarchy.h"
#include "semantic/label_index.h"
#include "store/triple_store.h"

namespace ontodb {

// Three ways to answer "which x have a type at or below C" (the RDFS reading
// of `?x rdf:type C`), compared in benchmarks/semantic/ and in
// docs/semantic-search.md section 3.
//
//   Intervals    index every rdf:type triple once as (label of its class,
//                instance); a query is one range scan per interval of C.
//   Rewrite      no extra storage; one scan of the store per class below C.
//   Materialize  add (x type A) for every ancestor A of each type of x, then a
//                query is one scan. Plan 13's choice.
//
// All three return the same set, ascending and without duplicates.

// The (class, parent) pairs of every `predicate` triple in the store, and the
// classes that are the object of an `rdf:type` triple but sit in no edge.
auto CollectSubClassEdges(TripleStore* store, term_id_t sub_class_of) -> std::vector<std::pair<term_id_t, term_id_t>>;
auto CollectTypeClasses(TripleStore* store, term_id_t rdf_type) -> std::vector<term_id_t>;

// Every (label(C), x) for a triple (x rdf:type C) whose class is in `h`.
auto TypeLabelPairs(const ClassHierarchy& h, TripleStore* store, term_id_t rdf_type)
    -> std::vector<std::pair<uint32_t, term_id_t>>;

auto InstancesByIntervals(const ClassHierarchy& h, LabelIndex* index, term_id_t cls,
                          LabelIndex::ScanStats* stats = nullptr) -> std::vector<term_id_t>;
auto InstancesByRewrite(const ClassHierarchy& h, TripleStore* store, term_id_t rdf_type, term_id_t cls,
                        size_t* scans = nullptr) -> std::vector<term_id_t>;
// Inserts the entailed type triples; returns how many were new.
auto MaterializeTypes(const ClassHierarchy& h, TripleStore* store, term_id_t rdf_type) -> size_t;
auto InstancesByScan(TripleStore* store, term_id_t rdf_type, term_id_t cls) -> std::vector<term_id_t>;

}  // namespace ontodb
