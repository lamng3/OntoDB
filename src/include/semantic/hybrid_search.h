#pragma once

#include <vector>

#include "semantic/class_hierarchy.h"
#include "semantic/flat_index.h"
#include "semantic/hnsw_index.h"
#include "semantic/ivf_index.h"
#include "semantic/label_index.h"
#include "semantic/vector_store.h"

namespace ontodb {

// "The k terms nearest q that lie at or under class C": a vector search
// filtered by a subtree of the hierarchy. docs/semantic-search.md section 4.
//
// Let s be the share of the vectors that lie under C.
//   kPreFilter      Resolve C to its members first (one B+ tree range scan per
//                   interval of C), then score only those, exactly. Cost about
//                   s * n * dim. Best when s is small.
//   kPostFilter*    Search the vector index for more than k results, keep those
//                   under C (a membership test is a binary search over C's
//                   intervals, no scan), and ask for four times as many again
//                   until k remain or the index is exhausted. Cost grows as s
//                   falls, unless the neighbors of q lie under C anyway.
//   kMask*          Resolve C as for pre-filtering, but hand the set to the
//                   index as a mask, so the walk or the list scan skips what is
//                   outside C. HNSW still crosses the masked rows.
//   kAuto           kPreFilter when C has at most `pre_threshold` classes,
//                   otherwise kMaskHnsw if there is an HNSW index, else
//                   kMaskIvf, else kPreFilter.
class HybridSearch {
 public:
  enum class Strategy { kAuto, kPreFilter, kPostFilterHnsw, kPostFilterIvf, kMaskHnsw, kMaskIvf };

  struct Parts {
    const ClassHierarchy* hierarchy{nullptr};
    LabelIndex* classes{nullptr};  // (label, class) for every class in the hierarchy
    const VectorStore* vectors{nullptr};
    const FlatIndex* flat{nullptr};
    IvfIndex* ivf{nullptr};
    const HnswIndex* hnsw{nullptr};
  };

  struct Params {
    uint32_t nprobe{16};
    uint32_t ef{64};
    size_t pre_threshold{4096};
    size_t max_visits{0};  // HNSW walk cap under a mask; 0 is none
  };

  struct Result {
    std::vector<Neighbor> neighbors;
    Strategy used{Strategy::kAuto};
    size_t members{0};  // classes the filter resolved to (0 when it was not resolved)
    size_t rounds{0};   // index searches made
    LabelIndex::ScanStats scans;
    SearchStats stats;
  };

  explicit HybridSearch(const Parts& parts) : parts_(parts) {}

  // `under == INVALID_TERM_ID` means no filter. A class that is not in the
  // hierarchy has no members, so the answer is empty.
  auto Similar(const float* query, size_t k, term_id_t under, Strategy strategy, const Params& params) const
      -> Result;

  static auto Name(Strategy s) -> const char*;

 private:
  // Rows of the vectors that belong to the members of `under`.
  auto Members(term_id_t under, Result* out) const -> std::vector<uint32_t>;

  Parts parts_;
};

}  // namespace ontodb
