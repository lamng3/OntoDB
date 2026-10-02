#pragma once

#include <filesystem>
#include <string>

#include "dictionary/dictionary.h"
#include "store/triple_store.h"

namespace ontodb {

struct LoadStats {
  uint64_t triples{0};
  double milliseconds{0};
};

// Parses one RDF file into `dictionary` and `store`. The syntax comes from
// the extension: .ttl Turtle, .nt N-Triples, .owl and .rdf RDF/XML.
// OWL is stored as triples. This loader does not reason.
// `triples` counts statements in the file, including ones already stored.
class RdfLoader {
 public:
  auto Load(const std::filesystem::path& path, Dictionary& dictionary, TripleStore& store) const
      -> LoadStats;
};

}  // namespace ontodb
