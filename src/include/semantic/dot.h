#pragma once

#include <cstddef>
#include <cstdint>

namespace ontodb {

// Dot products with eight independent partial sums. A plain `sum += a[i] * b[i]`
// loop is a chain of dependent additions that a compiler may not reorder
// without -ffast-math, so it runs at scalar speed. Eight accumulators let it
// use the vector unit; the result differs from the plain loop only in the last
// bits.
inline auto Dot(const float* a, const float* b, size_t n) -> float {
  float s[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    for (size_t j = 0; j < 8; j++) {
      s[j] += a[i + j] * b[i + j];
    }
  }
  float tail = 0;
  for (; i < n; i++) {
    tail += a[i] * b[i];
  }
  return (((s[0] + s[1]) + (s[2] + s[3])) + ((s[4] + s[5]) + (s[6] + s[7]))) + tail;
}

// A float vector against 8-bit codes (the caller applies the scale).
inline auto DotCodes(const float* q, const int8_t* c, size_t n) -> float {
  float s[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    for (size_t j = 0; j < 8; j++) {
      s[j] += q[i + j] * static_cast<float>(c[i + j]);
    }
  }
  float tail = 0;
  for (; i < n; i++) {
    tail += q[i] * static_cast<float>(c[i]);
  }
  return (((s[0] + s[1]) + (s[2] + s[3])) + ((s[4] + s[5]) + (s[6] + s[7]))) + tail;
}

}  // namespace ontodb
