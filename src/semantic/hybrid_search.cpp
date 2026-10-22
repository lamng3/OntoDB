#include "semantic/hybrid_search.h"

#include <algorithm>

namespace ontodb {

HybridSearch::HybridSearch(const Parts& parts) : parts_(parts) {
  row_label_.assign(parts_.vectors->Size(), 0);
  for (uint32_t row = 0; row < row_label_.size(); row++) {
    row_label_[row] = parts_.hierarchy->Label(parts_.vectors->IdAt(row));
  }
}

auto HybridSearch::Name(Strategy s) -> const char* {
  switch (s) {
    case Strategy::kAuto: return "auto";
    case Strategy::kPreFilter: return "pre-filter";
    case Strategy::kPostFilterHnsw: return "post-filter/hnsw";
    case Strategy::kPostFilterIvf: return "post-filter/ivf";
    case Strategy::kMaskHnsw: return "mask/hnsw";
    case Strategy::kMaskIvf: return "mask/ivf";
  }
  return "?";
}

auto HybridSearch::Members(term_id_t under, Result* out) const -> std::vector<uint32_t> {
  std::vector<uint32_t> rows;
  const auto ids = parts_.classes->Scan(parts_.hierarchy->Intervals(under), &out->scans);
  rows.reserve(ids.size());
  for (const term_id_t id : ids) {
    if (const auto row = parts_.vectors->Row(id); row.has_value()) {
      rows.push_back(*row);
    }
  }
  out->members = rows.size();
  return rows;
}

auto HybridSearch::Similar(const float* query, size_t k, term_id_t under, Strategy strategy, const Params& params) const
    -> Result {
  Result out;
  const size_t n = parts_.vectors->Size();
  const bool filtered = under != INVALID_TERM_ID;
  if (filtered && !parts_.hierarchy->Contains(under)) {
    out.used = strategy;
    return out;
  }
  if (strategy == Strategy::kAuto) {
    if (!filtered) {
      strategy = parts_.hnsw != nullptr ? Strategy::kMaskHnsw : parts_.ivf != nullptr ? Strategy::kMaskIvf : Strategy::kPreFilter;
    } else if (parts_.hierarchy->DescendantCount(under) <= params.pre_threshold || (parts_.hnsw == nullptr && parts_.ivf == nullptr)) {
      strategy = Strategy::kPreFilter;
    } else {
      strategy = parts_.hnsw != nullptr ? Strategy::kMaskHnsw : Strategy::kMaskIvf;
    }
  }
  out.used = strategy;
  if (!filtered) {
    // No filter: the index alone, or the whole scan when there is none.
    out.rounds = 1;
    if (strategy == Strategy::kPreFilter || (parts_.hnsw == nullptr && parts_.ivf == nullptr)) {
      out.neighbors = parts_.flat->Search(query, k, &out.stats);
    } else if (strategy == Strategy::kMaskIvf || strategy == Strategy::kPostFilterIvf || parts_.hnsw == nullptr) {
      out.neighbors = parts_.ivf->Search(query, k, params.nprobe, nullptr, &out.stats);
    } else {
      out.neighbors = parts_.hnsw->Search(query, k, params.ef, nullptr, &out.stats, params.max_visits);
    }
    return out;
  }
  switch (strategy) {
    case Strategy::kPreFilter: {
      const auto rows = Members(under, &out);
      out.rounds = 1;
      out.neighbors = parts_.flat->SearchRows(query, k, rows, &out.stats);
      break;
    }
    case Strategy::kMaskHnsw:
    case Strategy::kMaskIvf: {
      const RowFilter filter(&row_label_, &parts_.hierarchy->Intervals(under));
      out.rounds = 1;
      if (strategy == Strategy::kMaskHnsw) {
        out.neighbors = parts_.hnsw->Search(query, k, params.ef, &filter, &out.stats, params.max_visits);
      } else {
        out.neighbors = parts_.ivf->Search(query, k, params.nprobe, &filter, &out.stats);
      }
      break;
    }
    case Strategy::kPostFilterHnsw:
    case Strategy::kPostFilterIvf:
    case Strategy::kAuto: {
      size_t want = std::max<size_t>(k * 4, k);
      while (true) {
        out.rounds++;
        std::vector<Neighbor> got;
        if (strategy == Strategy::kPostFilterHnsw) {
          got = parts_.hnsw->Search(query, want, std::max<uint32_t>(params.ef, static_cast<uint32_t>(want)), nullptr, &out.stats);
        } else {
          got = parts_.ivf->Search(query, want, params.nprobe, nullptr, &out.stats);
        }
        out.neighbors.clear();
        for (const Neighbor& nb : got) {
          if (parts_.hierarchy->IsDescendant(nb.id, under)) {
            out.neighbors.push_back(nb);
            if (out.neighbors.size() == k) {
              break;
            }
          }
        }
        if (out.neighbors.size() >= k || want >= n || got.size() < want) {
          break;
        }
        want *= 4;
      }
      break;
    }
  }
  return out;
}

}  // namespace ontodb
