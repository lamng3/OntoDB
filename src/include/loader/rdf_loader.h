#pragma once

#include <filesystem>
#include <string>
#include <vector>

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
//
// Terms go to the dictionary as they are parsed; the triples go to the store
// in one InsertBatch at the end, so a store that can bulk-load does.
// LoadFiles parses several files and hands all their triples over in one
// batch, which lets an empty indexed store bulk-load a multi-file dataset.
class RdfLoader {
 public:
  auto Load(const std::filesystem::path& path, Dictionary& dictionary,
            TripleStore& store) const -> LoadStats;
  auto LoadFiles(const std::vector<std::filesystem::path>& paths, Dictionary& dictionary,
                 TripleStore& store) const -> LoadStats;

 private:
  // Parses one file into `batch`, interning terms; returns the statement count.
  auto Parse(const std::filesystem::path& path, Dictionary& dictionary,
             std::vector<Triple>* batch) const -> uint64_t;
};

}  // namespace ontodb
