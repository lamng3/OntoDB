#pragma once

#include <cstdint>
#include <vector>

namespace ontodb {

// Product quantization (Jegou, Douze, Schmid, TPAMI 2011). A vector of `dim`
// floats is cut into `m` sub-vectors of dim / m floats; each is replaced by
// the index of its nearest of 256 learned centers, so a vector costs m bytes.
// A query is scored by asymmetric distance computation: build one table per
// sub-space of the dot products of the query's sub-vector with the 256
// centers (256 * dim multiply-adds), then a stored code's score is the sum of
// m table entries. That equals the dot product of the query with the decoded
// vector, exactly, so the only error is the quantization of the stored side.
//
// `dim` must be a multiple of `m`. Not thread-safe while training.
class ProductQuantizer {
 public:
  ProductQuantizer(uint32_t dim, uint32_t m);

  auto M() const -> uint32_t { return m_; }
  auto Dim() const -> uint32_t { return dim_; }

  // Learns the centers from `n` row-major vectors.
  void Train(const float* data, size_t n, uint32_t iterations, uint64_t seed);
  auto Trained() const -> bool { return !centers_.empty(); }
  // m bytes for one vector.
  void Encode(const float* v, uint8_t* code) const;
  void Decode(const uint8_t* code, float* v) const;
  // m * 256 floats, the dot products of `query` with every center.
  auto Table(const float* query) const -> std::vector<float>;
  static auto Score(const std::vector<float>& table, const uint8_t* code, uint32_t m) -> float {
    float s = 0;
    for (uint32_t j = 0; j < m; j++) {
      s += table[static_cast<size_t>(j) * 256 + code[j]];
    }
    return s;
  }

 private:
  uint32_t dim_;
  uint32_t m_;
  uint32_t dsub_;
  std::vector<float> centers_;  // m * 256 * dsub
};

}  // namespace ontodb
