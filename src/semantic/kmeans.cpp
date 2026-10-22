#include "semantic/kmeans.h"

#include "semantic/dot.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace ontodb {

namespace {

void Unit(float* v, uint32_t dim) {
  const float norm = std::sqrt(Dot(v, v, dim));
  if (norm > 0) {
    for (uint32_t i = 0; i < dim; i++) {
      v[i] /= norm;
    }
  }
}

}  // namespace

auto NearestCenter(const float* centers, uint32_t k, uint32_t dim, const float* v, bool spherical, float* score)
    -> uint32_t {
  uint32_t best = 0;
  float best_score = -INFINITY;
  for (uint32_t c = 0; c < k; c++) {
    const float* ctr = centers + static_cast<size_t>(c) * dim;
    float s;
    if (spherical) {
      s = Dot(v, ctr, dim);
    } else {
      s = 0;
      for (uint32_t i = 0; i < dim; i++) {
        const float d = v[i] - ctr[i];
        s -= d * d;
      }
    }
    if (s > best_score) {
      best_score = s;
      best = c;
    }
  }
  if (score != nullptr) {
    *score = best_score;
  }
  return best;
}

auto KMeans(const float* data, size_t n, uint32_t dim, uint32_t k, uint32_t iterations, uint64_t seed,
            bool spherical) -> std::vector<float> {
  std::vector<float> centers(static_cast<size_t>(k) * dim, 0.0f);
  if (n == 0 || k == 0) {
    return centers;
  }
  std::mt19937_64 rng(seed);
  if (n <= k) {
    for (uint32_t c = 0; c < k; c++) {
      const size_t row = std::min<size_t>(c, n - 1);
      std::copy(data + row * dim, data + (row + 1) * dim, centers.begin() + static_cast<size_t>(c) * dim);
    }
    return centers;
  }
  // k-means++ seeding: each new center is a row drawn with probability
  // proportional to its squared distance from the nearest center so far.
  auto dist2 = [&](const float* a, const float* b) {
    if (spherical) {
      return std::max(0.0f, 2.0f - 2.0f * Dot(a, b, dim));
    }
    float d = 0;
    for (uint32_t i = 0; i < dim; i++) {
      const float t = a[i] - b[i];
      d += t * t;
    }
    return d;
  };
  std::vector<float> nearest(n, INFINITY);
  size_t pick = rng() % n;
  for (uint32_t c = 0; c < k; c++) {
    float* ctr = centers.data() + static_cast<size_t>(c) * dim;
    std::copy(data + pick * dim, data + (pick + 1) * dim, ctr);
    double total = 0;
    for (size_t row = 0; row < n; row++) {
      nearest[row] = std::min(nearest[row], dist2(data + row * dim, ctr));
      total += nearest[row];
    }
    if (c + 1 == k) {
      break;
    }
    if (total <= 0) {
      pick = rng() % n;
      continue;
    }
    double target = std::uniform_real_distribution<double>(0.0, total)(rng);
    pick = n - 1;
    for (size_t row = 0; row < n; row++) {
      target -= nearest[row];
      if (target <= 0) {
        pick = row;
        break;
      }
    }
  }
  std::vector<uint32_t> assign(n, 0);
  std::vector<float> sums(static_cast<size_t>(k) * dim);
  std::vector<size_t> counts(k);
  for (uint32_t it = 0; it < iterations; it++) {
    std::fill(sums.begin(), sums.end(), 0.0f);
    std::fill(counts.begin(), counts.end(), 0);
    for (size_t row = 0; row < n; row++) {
      const float* v = data + row * dim;
      const uint32_t c = NearestCenter(centers.data(), k, dim, v, spherical);
      assign[row] = c;
      counts[c]++;
      float* sum = sums.data() + static_cast<size_t>(c) * dim;
      for (uint32_t i = 0; i < dim; i++) {
        sum[i] += v[i];
      }
    }
    for (uint32_t c = 0; c < k; c++) {
      float* ctr = centers.data() + static_cast<size_t>(c) * dim;
      if (counts[c] == 0) {
        const size_t row = rng() % n;
        std::copy(data + row * dim, data + (row + 1) * dim, ctr);
        continue;
      }
      const float* sum = sums.data() + static_cast<size_t>(c) * dim;
      for (uint32_t i = 0; i < dim; i++) {
        ctr[i] = sum[i] / static_cast<float>(counts[c]);
      }
      if (spherical) {
        Unit(ctr, dim);
      }
    }
  }
  return centers;
}

}  // namespace ontodb
