#pragma once

#include <cstdint>
#include <vector>

#include "buffer/buffer_pool_manager.h"
#include "semantic/flat_index.h"
#include "semantic/product_quantizer.h"
#include "storage/index/b_plus_tree.h"

namespace ontodb {

// An inverted-file index (Jegou et al., TPAMI 2011) whose posting lists are
// ranges of a B+ tree. Spherical k-means gives `lists` centroids; every row of
// the vector store goes to its nearest centroid; the pair is the key
// (list, row, 0). A query scores the centroids, takes the `nprobe` nearest
// lists, and reads each as a prefix scan of the tree, scoring the rows it finds.
//
// Cost, per query: lists * dim for the centroids plus nprobe * (n / lists) *
// dim for the rows; minimal at lists = sqrt(nprobe * n). Recall rises with
// nprobe / lists, and nprobe = lists is exact search over the same codes.
//
// The rows are positions in the store, so the index is derived data: rebuild
// it when the store changes. `encoding` is how the scored vectors are kept:
// float32 (exact scores), sq8 (a quarter of the memory), or pq (m bytes per
// vector, scored through a table).
class IvfIndex {
 public:
  enum class Encoding { kFloat32, kSq8, kPq };

  struct Options {
    uint32_t lists{0};         // 0: about 4 * sqrt(n)
    uint32_t iterations{10};   // k-means rounds
    uint64_t seed{1};
    size_t max_train{0};       // rows the centroids are trained on; 0: 64 * lists
    Encoding encoding{Encoding::kFloat32};
    uint32_t pq_m{48};         // sub-vectors, when encoding is pq
  };

  IvfIndex(const VectorStore* store, BufferPoolManager* bpm, const Options& options);

  void Build();
  auto Built() const -> bool { return !centroids_.empty(); }
  auto Lists() const -> uint32_t { return lists_; }
  auto Tree() -> BPlusTree* { return &tree_; }
  // Bytes of the structures a query reads besides the B+ tree pages: the
  // centroids and the vector codes.
  auto Bytes() const -> size_t;

  // The k best among the probed lists, best first. `allowed` restricts the
  // rows that may be returned; rows it rules out are skipped before scoring.
  auto Search(const float* query, size_t k, uint32_t nprobe, const RowMask* allowed = nullptr,
              SearchStats* stats = nullptr) -> std::vector<Neighbor>;

 private:
  const VectorStore* store_;
  Options options_;
  uint32_t lists_{0};
  page_id_t header_;
  BPlusTree tree_;
  std::vector<float> centroids_;
  FlatIndex flat_;
  ProductQuantizer pq_;
  std::vector<uint8_t> codes_;
};

}  // namespace ontodb
