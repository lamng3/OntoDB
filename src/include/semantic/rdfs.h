#pragma once

#include <set>

#include "common/types.h"
#include "dictionary/dictionary.h"

namespace ontodb {

// The RDFS closure of `triples`: subClassOf and subPropertyOf are transitive,
// types move up subClassOf, triples move up subPropertyOf, and domain and
// range add types. The result includes `triples`. Terms the dictionary does
// not have cannot appear in a rule, so a store without the RDFS vocabulary
// is unchanged.
auto RdfsClosure(const std::set<Triple>& triples, Dictionary& dictionary) -> std::set<Triple>;

}  // namespace ontodb
