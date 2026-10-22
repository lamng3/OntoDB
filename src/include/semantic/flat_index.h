#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "semantic/class_hierarchy.h"
#include "semantic/vector_store.h"

namespace ontodb {

struct Neighbor {
  term_id_t id{0};
  float score{0};  // cosine similarity, 1 is the same direction
};

struct SearchStats {
  size_t scored{0};  // vectors whose similarity was computed
  size_t lists{0};   // IVF lists probed
  size_t keys{0};    // IVF list entries read
  size_t hops{0};    // HNSW nodes expanded
};

// A mask over store rows: 1 means the row may be returned.
using RowMask = std::vector<uint8_t>;

// Which rows a search may return. Either an explicit mask, or a predicate that
// a row is allowed when its label lies in one of a class's intervals. The
// second form resolves nothing in advance: each row a search touches costs one
// binary search over the intervals, instead of a scan of the whole subtree.
// A null `const RowFilter*` means every row is allowed.
struct RowFilter {
  const RowMask* mask{nullptr};
  const std::vector<uint32_t>* row_label{nullptr};  // label of each row, 0 when it has none
  const std::vector<Interval>* intervals{nullptr};

  RowFilter() = default;
  RowFilter(const RowMask* m) : mask(m) {}  // NOLINT: a mask is the common case
  RowFilter(const std::vector<uint32_t>* labels, const std::vector<Interval>* ivs) : row_label(labels), intervals(ivs) {}

  auto Allows(uint32_t row) const -> bool {
    if (mask != nullptr) {
      return (*mask)[row] != 0;
    }
    const uint32_t label = (*row_label)[row];
    if (label == 0) {
      return false;
    }
    const auto it = std::upper_bound(intervals->begin(), intervals->end(), label,
                                     [](uint32_t value, const Interval& iv) { return value < iv.lo; });
    return it != intervals->begin() && label <= (it - 1)->hi;
  }
};

// Exact nearest neighbors by scoring every vector, optionally from 8-bit
// codes. The reference every other index is measured against.
//
// kSq8 stores each vector as dim signed bytes and one float scale, s = max|x|
// / 127, so a coordinate is off by at most s / 2 and, for a unit query, a
// score is off by at most (s / 2) * sqrt(dim). A quarter of the memory of
// kFloat32; ranks can swap when two scores are closer than that bound.
//
// The index reads the store's rows at Search time for kFloat32 and keeps its
// own copy of the codes for kSq8, so build it after the vectors are loaded.
class FlatIndex {
 public:
  enum class Precision { kFloat32, kSq8 };

  FlatIndex(const VectorStore* store, Precision precision);

  // The k best by score, best first. `query` has Dim() floats and need not be
  // normalized.
  auto Search(const float* query, size_t k, SearchStats* stats = nullptr) const -> std::vector<Neighbor>;
  // The same, among the given rows only.
  auto SearchRows(const float* query, size_t k, const std::vector<uint32_t>& rows,
                  SearchStats* stats = nullptr) const -> std::vector<Neighbor>;
  auto Score(const float* query, uint32_t row) const -> float;
  auto Bytes() const -> size_t;
  auto GetPrecision() const -> Precision { return precision_; }

 private:
  const VectorStore* store_;
  Precision precision_;
  std::vector<int8_t> codes_;
  std::vector<float> scales_;
};

// A unit-length copy of `v` (dim floats).
auto Normalized(const float* v, uint32_t dim) -> std::vector<float>;

}  // namespace ontodb
