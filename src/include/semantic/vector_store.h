#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/types.h"
#include "dictionary/dictionary.h"

namespace ontodb {

// One embedding per term id, held in memory as a dense row-major matrix of
// unit-length float32 vectors, so cosine similarity is a dot product. Rows are
// numbered in the order terms were first put; a term put twice keeps its row
// and its vector is replaced.
//
// A vector file is "OVEC2\n", uint32 dimension, uint64 count, then per record
// a uint32 length, the term's spelling, and `dimension` little-endian float32
// values, then a CRC-32C of everything before it. "OVEC1\n" is the same
// without the checksum (scripts/embed-terms writes it). Files name terms by
// spelling, so they survive a dictionary that numbers terms differently.
//
// Not thread-safe for writers; any number of threads may read.
class VectorStore {
 public:
  explicit VectorStore(uint32_t dim) : dim_(dim) {}

  struct LoadStats {
    size_t read{0};
    size_t stored{0};
    size_t skipped_unknown{0};  // spelling not in the dictionary
  };

  // The dimension recorded in a vector file's header.
  static auto PeekDimension(const std::string& path) -> uint32_t;

  auto Dim() const -> uint32_t { return dim_; }
  auto Size() const -> size_t { return ids_.size(); }

  // Normalizes `v` (dim floats) and stores it. Throws std::invalid_argument
  // for a zero or non-finite vector.
  void Put(term_id_t id, const float* v);
  auto Row(term_id_t id) const -> std::optional<uint32_t>;
  auto Vector(uint32_t row) const -> const float* { return data_.data() + static_cast<size_t>(row) * dim_; }
  auto IdAt(uint32_t row) const -> term_id_t { return ids_[row]; }

  // Writes every vector, naming each term through `dictionary`. A term the
  // dictionary no longer knows is an error.
  void Save(const std::string& path, const Dictionary& dictionary) const;
  // Reads a file into this store (its dimension must match). A spelling the
  // dictionary lacks is skipped, or interned when `intern` is true. Throws
  // StorageException for a bad magic, a short read, or a checksum mismatch.
  auto Load(const std::string& path, Dictionary* dictionary, bool intern = false) -> LoadStats;

 private:
  uint32_t dim_;
  std::vector<float> data_;
  std::vector<term_id_t> ids_;
  std::unordered_map<term_id_t, uint32_t> row_of_;
};

}  // namespace ontodb
