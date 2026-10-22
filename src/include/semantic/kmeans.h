#pragma once

#include <cstdint>
#include <vector>

namespace ontodb {

// Lloyd's k-means on `n` row-major vectors of `dim` floats. Centers are seeded
// by k-means++ (seeded, so a run is reproducible). An empty cluster
// is re-seeded from a random row. With `spherical`, rows and centers are unit
// length and a row goes to the center with the largest dot product (cosine);
// otherwise to the nearest by squared Euclidean distance. Returns k * dim
// floats. If n < k the first n rows are the centers and the rest repeat the
// last.
auto KMeans(const float* data, size_t n, uint32_t dim, uint32_t k, uint32_t iterations, uint64_t seed,
            bool spherical) -> std::vector<float>;

// The center nearest `v` under the same rule, and its score (the dot product
// when spherical, the negated squared distance otherwise).
auto NearestCenter(const float* centers, uint32_t k, uint32_t dim, const float* v, bool spherical,
                   float* score = nullptr) -> uint32_t;

}  // namespace ontodb
