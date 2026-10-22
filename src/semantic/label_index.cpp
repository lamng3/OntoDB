#include "semantic/label_index.h"

#include <algorithm>

#include "storage/page/b_plus_tree_header_page.h"

namespace ontodb {

namespace {

auto NewHeader(BufferPoolManager* bpm) -> page_id_t {
  page_id_t header = INVALID_PAGE_ID;
  WritePageGuard guard = bpm->NewPageGuarded(&header);
  BPlusTreeHeaderPage(guard.GetPage()).Init();
  return header;
}

}  // namespace

LabelIndex::LabelIndex(BufferPoolManager* bpm) : LabelIndex(bpm, NewHeader(bpm)) {}

LabelIndex::LabelIndex(BufferPoolManager* bpm, page_id_t header)
    : header_(header), tree_("labels", header, bpm) {}

auto LabelIndex::Key(uint32_t label, term_id_t member) -> TripleKey {
  return TripleKey::Encode(label, member, 0);
}

auto LabelIndex::Insert(uint32_t label, term_id_t member) -> bool {
  return tree_.Insert(Key(label, member));
}

auto LabelIndex::Remove(uint32_t label, term_id_t member) -> bool {
  return tree_.Remove(Key(label, member));
}

void LabelIndex::BulkLoad(std::vector<std::pair<uint32_t, term_id_t>> pairs) {
  std::sort(pairs.begin(), pairs.end());
  pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
  std::vector<TripleKey> keys;
  keys.reserve(pairs.size());
  for (const auto& [label, member] : pairs) {
    keys.push_back(Key(label, member));
  }
  tree_.BulkLoad(keys);
}

auto LabelIndex::Scan(const std::vector<Interval>& intervals, ScanStats* stats) -> std::vector<term_id_t> {
  std::vector<term_id_t> out;
  for (const Interval& iv : intervals) {
    // [ (lo, 0, 0), (hi + 1, 0, 0) ): every key whose label is in [lo, hi].
    const TripleKey lower = Key(iv.lo, 0);
    const TripleKey upper = Key(iv.hi + 1, 0);
    if (stats != nullptr) {
      stats->scans++;
    }
    for (auto it = tree_.BeginRange(lower, upper); !it.IsEnd(); ++it) {
      term_id_t label = 0, member = 0, unused = 0;
      (*it).Decode(&label, &member, &unused);
      out.push_back(member);
      if (stats != nullptr) {
        stats->keys++;
      }
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

}  // namespace ontodb
