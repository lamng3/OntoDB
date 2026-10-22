#include "semantic/hnsw_index.h"

#include "semantic/dot.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <random>

namespace ontodb {

namespace {

// One stamp array per thread, so concurrent queries do not share marks.
struct Visited {
  std::vector<uint32_t> stamp;
  uint32_t epoch{0};

  void Reset(size_t n) {
    if (stamp.size() < n) {
      stamp.assign(n, 0);
      epoch = 0;
    }
    if (++epoch == 0) {
      std::fill(stamp.begin(), stamp.end(), 0);
      epoch = 1;
    }
  }
  auto Seen(uint32_t row) -> bool {
    if (stamp[row] == epoch) {
      return true;
    }
    stamp[row] = epoch;
    return false;
  }
};

thread_local Visited g_visited;

}  // namespace

HnswIndex::HnswIndex(const VectorStore* store, const Options& options) : store_(store), options_(options) {}

auto HnswIndex::Sim(uint32_t a, uint32_t b) const -> float {
  return SimQ(store_->Vector(a), b);
}

auto HnswIndex::SimQ(const float* q, uint32_t row) const -> float {
  return Dot(q, store_->Vector(row), store_->Dim());
}

auto HnswIndex::SearchLayer(const float* q, std::vector<Cand> entries, uint32_t ef, int layer,
                            const RowMask* allowed, SearchStats* stats, size_t max_visits) const
    -> std::vector<Cand> {
  g_visited.Reset(store_->Size());
  auto worse = [](const Cand& a, const Cand& b) { return a.score < b.score; };  // max-heap: best on top
  auto better = [](const Cand& a, const Cand& b) { return a.score > b.score; };  // min-heap: worst on top
  std::priority_queue<Cand, std::vector<Cand>, decltype(worse)> frontier(worse);
  std::priority_queue<Cand, std::vector<Cand>, decltype(better)> found(better);
  size_t visits = 0;
  size_t scored = 0;
  for (const Cand& e : entries) {
    if (g_visited.Seen(e.row)) {
      continue;
    }
    frontier.push(e);
    if (allowed == nullptr || (*allowed)[e.row] != 0) {
      found.push(e);
    }
  }
  while (!frontier.empty()) {
    const Cand c = frontier.top();
    if (found.size() >= ef && c.score < found.top().score) {
      break;
    }
    frontier.pop();
    if (max_visits != 0 && visits >= max_visits) {
      break;
    }
    visits++;
    for (const uint32_t nb : links_[c.row][std::min<size_t>(layer, links_[c.row].size() - 1)]) {
      if (g_visited.Seen(nb)) {
        continue;
      }
      const float s = SimQ(q, nb);
      scored++;
      const bool ok = allowed == nullptr || (*allowed)[nb] != 0;
      if (found.size() < ef || s > found.top().score || !ok) {
        frontier.push({s, nb});
        if (ok) {
          found.push({s, nb});
          if (found.size() > ef) {
            found.pop();
          }
        }
      }
    }
  }
  if (stats != nullptr) {
    stats->scored += scored;
    stats->hops += visits;
  }
  std::vector<Cand> out(found.size());
  for (size_t i = out.size(); i-- > 0;) {
    out[i] = found.top();
    found.pop();
  }
  return out;
}

auto HnswIndex::Select(const std::vector<Cand>& sorted, size_t limit) const -> std::vector<uint32_t> {
  std::vector<uint32_t> kept;
  std::vector<uint32_t> pruned;
  for (const Cand& c : sorted) {
    if (kept.size() >= limit) {
      break;
    }
    bool good = true;
    for (const uint32_t r : kept) {
      if (Sim(c.row, r) > c.score) {
        good = false;
        break;
      }
    }
    (good ? kept : pruned).push_back(c.row);
  }
  for (size_t i = 0; kept.size() < limit && i < pruned.size(); i++) {
    kept.push_back(pruned[i]);
  }
  return kept;
}

void HnswIndex::Insert(uint32_t row) {
  // The level is a function of (seed, row) so a build is reproducible.
  std::mt19937_64 gen(options_.seed * 0x9E3779B97F4A7C15ULL + row);
  const double u = std::max(std::uniform_real_distribution<double>(0.0, 1.0)(gen), 1e-12);
  const int level = static_cast<int>(std::floor(-std::log(u) / std::log(static_cast<double>(options_.m))));
  links_[row].assign(static_cast<size_t>(level) + 1, {});
  if (entry_ == kNone) {
    entry_ = row;
    top_ = level;
    return;
  }
  const float* q = store_->Vector(row);
  std::vector<Cand> eps{{SimQ(q, entry_), entry_}};
  for (int l = top_; l > level; l--) {
    eps = SearchLayer(q, eps, 1, l, nullptr, nullptr, 0);
  }
  for (int l = std::min(level, top_); l >= 0; l--) {
    const auto found = SearchLayer(q, eps, options_.ef_construction, l, nullptr, nullptr, 0);
    links_[row][l] = Select(found, options_.m);
    for (const uint32_t e : links_[row][l]) {
      auto& back = links_[e][l];
      back.push_back(row);
      if (back.size() > MaxLinks(l)) {
        std::vector<Cand> around;
        around.reserve(back.size());
        for (const uint32_t x : back) {
          around.push_back({Sim(e, x), x});
        }
        std::sort(around.begin(), around.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });
        back = Select(around, MaxLinks(l));
      }
    }
    eps = found;
  }
  if (level > top_) {
    entry_ = row;
    top_ = level;
  }
}

void HnswIndex::Build() {
  entry_ = kNone;
  top_ = -1;
  links_.assign(store_->Size(), {});
  for (uint32_t row = 0; row < store_->Size(); row++) {
    Insert(row);
  }
}

auto HnswIndex::Bytes() const -> size_t {
  size_t links = 0;
  for (const auto& layers : links_) {
    for (const auto& l : layers) {
      links += l.size();
    }
  }
  return links * sizeof(uint32_t);
}

auto HnswIndex::MeanDegree() const -> double {
  if (links_.empty()) {
    return 0;
  }
  size_t total = 0;
  for (const auto& layers : links_) {
    total += layers[0].size();
  }
  return static_cast<double>(total) / static_cast<double>(links_.size());
}

auto HnswIndex::Search(const float* query, size_t k, uint32_t ef, const RowMask* allowed, SearchStats* stats,
                       size_t max_visits) const -> std::vector<Neighbor> {
  if (entry_ == kNone || k == 0) {
    return {};
  }
  const auto q = Normalized(query, store_->Dim());
  std::vector<Cand> eps{{SimQ(q.data(), entry_), entry_}};
  for (int l = top_; l > 0; l--) {
    eps = SearchLayer(q.data(), eps, 1, l, nullptr, stats, 0);
  }
  const auto found = SearchLayer(q.data(), eps, std::max<uint32_t>(ef, static_cast<uint32_t>(k)), 0, allowed, stats, max_visits);
  std::vector<Neighbor> out;
  for (size_t i = 0; i < found.size() && out.size() < k; i++) {
    out.push_back({store_->IdAt(found[i].row), found[i].score});
  }
  return out;
}

}  // namespace ontodb
