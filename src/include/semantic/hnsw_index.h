#pragma once

#include <cstdint>
#include <vector>

#include "semantic/flat_index.h"

namespace ontodb {

// Hierarchical Navigable Small World graph (Malkov and Yashunin, TPAMI 2020)
// over the rows of a vector store, scored by dot product of unit vectors.
//
// Every row gets a random top layer l with P(l >= i) = M^-i; layer 0 holds
// all rows. A query descends greedily from the top entry point to layer 1,
// then runs a best-first search of width `ef` on layer 0. Insertion finds
// efConstruction candidates per layer and keeps at most M of them with the
// paper's diversity heuristic (a candidate is kept only if it is nearer the
// new row than to every neighbor already kept); layer 0 allows 2M links.
// Memory is O(n M) links. There is no worst-case bound on recall or time.
//
// A row mask is tested while walking: rows it rules out are still crossed
// (they keep the graph connected) but are never returned, and the search ends
// when `ef` allowed rows have been found and no candidate can improve them.
// When few rows are allowed the walk can cross much of the graph; `max_visits`
// caps that (0: no cap), and the result is then whatever was found.
//
// The graph is derived data: rebuild it when the store changes. Building is
// single-threaded; Search is const and may run on several threads.
class HnswIndex {
 public:
  struct Options {
    uint32_t m{16};
    uint32_t ef_construction{200};
    uint64_t seed{1};
  };

  HnswIndex(const VectorStore* store, const Options& options);

  void Build();
  auto Built() const -> bool { return entry_ != kNone; }
  auto TopLayer() const -> int { return top_; }
  auto Bytes() const -> size_t;
  // Mean links per row on layer 0.
  auto MeanDegree() const -> double;

  auto Search(const float* query, size_t k, uint32_t ef, const RowFilter* allowed = nullptr,
              SearchStats* stats = nullptr, size_t max_visits = 0) const -> std::vector<Neighbor>;

 private:
  static constexpr uint32_t kNone = static_cast<uint32_t>(-1);
  struct Cand {
    float score;
    uint32_t row;
  };

  auto Sim(uint32_t a, uint32_t b) const -> float;
  auto SimQ(const float* q, uint32_t row) const -> float;
  auto MaxLinks(int layer) const -> size_t { return layer == 0 ? 2 * options_.m : options_.m; }
  // Best-first search on one layer; returns up to ef candidates, best first.
  auto SearchLayer(const float* q, std::vector<Cand> entries, uint32_t ef, int layer, const RowFilter* allowed,
                   SearchStats* stats, size_t max_visits) const -> std::vector<Cand>;
  auto Select(const std::vector<Cand>& sorted_best_first, size_t limit) const -> std::vector<uint32_t>;
  void Insert(uint32_t row);

  const VectorStore* store_;
  Options options_;
  std::vector<std::vector<std::vector<uint32_t>>> links_;  // row, layer, neighbors
  uint32_t entry_{kNone};
  int top_{-1};
};

}  // namespace ontodb
