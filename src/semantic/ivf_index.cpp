#include "semantic/ivf_index.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <queue>
#include <random>

#include "semantic/dot.h"
#include "semantic/kmeans.h"
#include "storage/page/b_plus_tree_header_page.h"

namespace ontodb {

namespace {

auto NewHeader(BufferPoolManager* bpm) -> page_id_t {
  page_id_t header = INVALID_PAGE_ID;
  WritePageGuard guard = bpm->NewPageGuarded(&header);
  BPlusTreeHeaderPage(guard.GetPage()).Init();
  return header;
}

auto ListKey(uint32_t list, uint32_t row) -> TripleKey {
  return TripleKey::Encode(list, row, 0);
}

}  // namespace

IvfIndex::IvfIndex(const VectorStore* store, BufferPoolManager* bpm, const Options& options)
    : store_(store)
    , options_(options)
    , header_(NewHeader(bpm))
    , tree_("ivf", header_, bpm)
    , flat_(store, options.encoding == Encoding::kSq8 ? FlatIndex::Precision::kSq8 : FlatIndex::Precision::kFloat32)
    , pq_(store->Dim(), options.encoding == Encoding::kPq ? options.pq_m : 1) {}

void IvfIndex::Build() {
  const size_t n = store_->Size();
  const uint32_t dim = store_->Dim();
  lists_ = options_.lists != 0 ? options_.lists
                               : std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(4.0 * std::sqrt(static_cast<double>(n)))));
  lists_ = static_cast<uint32_t>(std::min<size_t>(lists_, std::max<size_t>(n, 1)));
  const size_t want = options_.max_train != 0 ? options_.max_train : static_cast<size_t>(lists_) * 64;
  const size_t train = std::min(n, want);
  std::vector<float> sample(train * dim);
  {
    std::vector<uint32_t> rows(n);
    std::iota(rows.begin(), rows.end(), 0);
    std::mt19937_64 rng(options_.seed);
    std::shuffle(rows.begin(), rows.end(), rng);
    for (size_t i = 0; i < train; i++) {
      std::copy(store_->Vector(rows[i]), store_->Vector(rows[i]) + dim, sample.begin() + static_cast<long>(i * dim));
    }
  }
  centroids_ = KMeans(sample.data(), train, dim, lists_, options_.iterations, options_.seed, true);
  std::vector<TripleKey> keys;
  keys.reserve(n);
  std::vector<std::pair<uint32_t, uint32_t>> assigned(n);
  for (uint32_t row = 0; row < n; row++) {
    assigned[row] = {NearestCenter(centroids_.data(), lists_, dim, store_->Vector(row), true), row};
  }
  std::sort(assigned.begin(), assigned.end());
  for (const auto& [list, row] : assigned) {
    keys.push_back(ListKey(list, row));
  }
  tree_.BulkLoad(keys);
  if (options_.encoding == Encoding::kPq) {
    pq_.Train(sample.data(), train, options_.iterations, options_.seed);
    codes_.resize(n * pq_.M());
    for (uint32_t row = 0; row < n; row++) {
      pq_.Encode(store_->Vector(row), codes_.data() + static_cast<size_t>(row) * pq_.M());
    }
  }
}

auto IvfIndex::Bytes() const -> size_t {
  size_t bytes = centroids_.size() * sizeof(float);
  switch (options_.encoding) {
    case Encoding::kPq:
      return bytes + codes_.size();
    default:
      return bytes + flat_.Bytes();
  }
}

auto IvfIndex::Search(const float* query, size_t k, uint32_t nprobe, const RowMask* allowed, SearchStats* stats)
    -> std::vector<Neighbor> {
  const uint32_t dim = store_->Dim();
  const auto q = Normalized(query, dim);
  nprobe = std::min(std::max<uint32_t>(nprobe, 1), lists_);
  std::vector<std::pair<float, uint32_t>> order(lists_);
  for (uint32_t c = 0; c < lists_; c++) {
    order[c] = {Dot(q.data(), centroids_.data() + static_cast<size_t>(c) * dim, dim), c};
  }
  std::partial_sort(order.begin(), order.begin() + nprobe, order.end(),
                    [](const auto& a, const auto& b) { return a.first > b.first; });
  std::vector<float> table;
  if (options_.encoding == Encoding::kPq) {
    table = pq_.Table(q.data());
  }
  using Entry = std::pair<float, uint32_t>;
  std::priority_queue<Entry, std::vector<Entry>, std::greater<>> heap;
  size_t scored = 0, keys = 0;
  for (uint32_t p = 0; p < nprobe && k > 0; p++) {
    const uint32_t list = order[p].second;
    for (auto it = tree_.BeginRange(ListKey(list, 0), ListKey(list + 1, 0)); !it.IsEnd(); ++it) {
      term_id_t l = 0, row64 = 0, unused = 0;
      (*it).Decode(&l, &row64, &unused);
      const uint32_t row = static_cast<uint32_t>(row64);
      keys++;
      if (allowed != nullptr && (*allowed)[row] == 0) {
        continue;
      }
      const float s = options_.encoding == Encoding::kPq
                          ? ProductQuantizer::Score(table, codes_.data() + static_cast<size_t>(row) * pq_.M(), pq_.M())
                          : flat_.Score(q.data(), row);
      scored++;
      if (heap.size() < k) {
        heap.push({s, row});
      } else if (s > heap.top().first) {
        heap.pop();
        heap.push({s, row});
      }
    }
  }
  if (stats != nullptr) {
    stats->scored += scored;
    stats->lists += nprobe;
    stats->keys += keys;
  }
  std::vector<Neighbor> out(heap.size());
  for (size_t i = out.size(); i-- > 0;) {
    out[i] = {store_->IdAt(heap.top().second), heap.top().first};
    heap.pop();
  }
  return out;
}

}  // namespace ontodb
