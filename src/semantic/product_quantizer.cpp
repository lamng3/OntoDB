#include "semantic/product_quantizer.h"

#include <stdexcept>

#include "semantic/kmeans.h"

namespace ontodb {

ProductQuantizer::ProductQuantizer(uint32_t dim, uint32_t m) : dim_(dim), m_(m), dsub_(m == 0 ? 0 : dim / m) {
  if (m == 0 || dim % m != 0) {
    throw std::invalid_argument("dimension must be a positive multiple of the number of sub-vectors");
  }
}

void ProductQuantizer::Train(const float* data, size_t n, uint32_t iterations, uint64_t seed) {
  centers_.assign(static_cast<size_t>(m_) * 256 * dsub_, 0.0f);
  std::vector<float> sub(n * dsub_);
  for (uint32_t j = 0; j < m_; j++) {
    for (size_t row = 0; row < n; row++) {
      std::copy(data + row * dim_ + static_cast<size_t>(j) * dsub_, data + row * dim_ + static_cast<size_t>(j + 1) * dsub_,
                sub.begin() + static_cast<long>(row * dsub_));
    }
    const auto c = KMeans(sub.data(), n, dsub_, 256, iterations, seed + j, false);
    std::copy(c.begin(), c.end(), centers_.begin() + static_cast<long>(static_cast<size_t>(j) * 256 * dsub_));
  }
}

void ProductQuantizer::Encode(const float* v, uint8_t* code) const {
  for (uint32_t j = 0; j < m_; j++) {
    code[j] = static_cast<uint8_t>(NearestCenter(centers_.data() + static_cast<size_t>(j) * 256 * dsub_, 256, dsub_,
                                                 v + static_cast<size_t>(j) * dsub_, false));
  }
}

void ProductQuantizer::Decode(const uint8_t* code, float* v) const {
  for (uint32_t j = 0; j < m_; j++) {
    const float* c = centers_.data() + (static_cast<size_t>(j) * 256 + code[j]) * dsub_;
    std::copy(c, c + dsub_, v + static_cast<size_t>(j) * dsub_);
  }
}

auto ProductQuantizer::Table(const float* query) const -> std::vector<float> {
  std::vector<float> table(static_cast<size_t>(m_) * 256);
  for (uint32_t j = 0; j < m_; j++) {
    const float* q = query + static_cast<size_t>(j) * dsub_;
    for (uint32_t c = 0; c < 256; c++) {
      const float* ctr = centers_.data() + (static_cast<size_t>(j) * 256 + c) * dsub_;
      float s = 0;
      for (uint32_t i = 0; i < dsub_; i++) {
        s += q[i] * ctr[i];
      }
      table[static_cast<size_t>(j) * 256 + c] = s;
    }
  }
  return table;
}

}  // namespace ontodb
