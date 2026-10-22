#include "semantic/flat_index.h"

#include "semantic/dot.h"

#include <algorithm>
#include <cmath>
#include <queue>

namespace ontodb {

auto Normalized(const float* v, uint32_t dim) -> std::vector<float> {
  double norm = 0;
  for (uint32_t i = 0; i < dim; i++) {
    norm += static_cast<double>(v[i]) * v[i];
  }
  std::vector<float> out(v, v + dim);
  if (norm > 0) {
    const float inv = static_cast<float>(1.0 / std::sqrt(norm));
    for (float& x : out) {
      x *= inv;
    }
  }
  return out;
}

FlatIndex::FlatIndex(const VectorStore* store, Precision precision) : store_(store), precision_(precision) {
  if (precision_ != Precision::kSq8) {
    return;
  }
  const uint32_t dim = store_->Dim();
  codes_.resize(store_->Size() * dim);
  scales_.resize(store_->Size());
  for (uint32_t row = 0; row < store_->Size(); row++) {
    const float* v = store_->Vector(row);
    float peak = 0;
    for (uint32_t i = 0; i < dim; i++) {
      peak = std::max(peak, std::fabs(v[i]));
    }
    const float scale = peak > 0 ? peak / 127.0f : 1.0f;
    scales_[row] = scale;
    for (uint32_t i = 0; i < dim; i++) {
      codes_[static_cast<size_t>(row) * dim + i] = static_cast<int8_t>(std::lround(v[i] / scale));
    }
  }
}

auto FlatIndex::Score(const float* query, uint32_t row) const -> float {
  const uint32_t dim = store_->Dim();
  if (precision_ == Precision::kFloat32) {
    return Dot(query, store_->Vector(row), dim);
  }
  return DotCodes(query, codes_.data() + static_cast<size_t>(row) * dim, dim) * scales_[row];
}

auto FlatIndex::Bytes() const -> size_t {
  if (precision_ == Precision::kSq8) {
    return codes_.size() + scales_.size() * sizeof(float);
  }
  return store_->Size() * store_->Dim() * sizeof(float);
}

namespace {

struct Entry {
  float score;
  uint32_t row;
  friend auto operator>(const Entry& a, const Entry& b) -> bool { return a.score > b.score; }
};

}  // namespace

template <typename Rows>
static auto Best(const FlatIndex& index, const VectorStore* store, const float* q, size_t k, const Rows& rows,
                 SearchStats* stats) -> std::vector<Neighbor> {
  std::priority_queue<Entry, std::vector<Entry>, std::greater<>> heap;
  if (k == 0) {
    return {};
  }
  size_t scored = 0;
  rows([&](uint32_t row) {
    const float s = index.Score(q, row);
    scored++;
    if (heap.size() < k) {
      heap.push({s, row});
    } else if (s > heap.top().score) {
      heap.pop();
      heap.push({s, row});
    }
  });
  if (stats != nullptr) {
    stats->scored += scored;
  }
  std::vector<Neighbor> out(heap.size());
  for (size_t i = out.size(); i-- > 0;) {
    out[i] = {store->IdAt(heap.top().row), heap.top().score};
    heap.pop();
  }
  return out;
}

auto FlatIndex::Search(const float* query, size_t k, SearchStats* stats) const -> std::vector<Neighbor> {
  const auto q = Normalized(query, store_->Dim());
  const uint32_t n = static_cast<uint32_t>(store_->Size());
  return Best(*this, store_, q.data(), k, [n](auto&& visit) {
    for (uint32_t row = 0; row < n; row++) {
      visit(row);
    }
  }, stats);
}

auto FlatIndex::SearchRows(const float* query, size_t k, const std::vector<uint32_t>& rows,
                           SearchStats* stats) const -> std::vector<Neighbor> {
  const auto q = Normalized(query, store_->Dim());
  return Best(*this, store_, q.data(), k, [&rows](auto&& visit) {
    for (const uint32_t row : rows) {
      visit(row);
    }
  }, stats);
}

}  // namespace ontodb
