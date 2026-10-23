#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "buffer/buffer_pool_manager.h"
#include "catalog/catalog.h"
#include "dictionary/dictionary.h"
#include "storage/index/b_plus_tree.h"
#include "storage/term/term_heap.h"

namespace ontodb {

// Persistent dictionary. Both directions survive restart: spelling to id and
// id to spelling. Insert of an existing spelling returns the original id and
// does not allocate another.
//
// Everything lives in pages, built from the engine's own parts:
//   - spellings in a TermHeap;
//   - id -> spelling: a B+ tree keyed (id, heap page, heap slot); a 1-prefix
//     scan on the id finds the record;
//   - spelling -> id: a B+ tree keyed (hash of the spelling, id, 0); a
//     1-prefix scan on the hash lists candidate ids, and each candidate's
//     spelling is compared, so hash collisions are harmless;
//   - a header page, named by the catalog's dictionary root:
//       offset 8 magic "ODIC", 12 term count (u64), 20 heap first page,
//       24 by-id tree header page, 28 by-hash tree header page.
// The hash is 64-bit FNV-1a, fixed so that files are portable between builds.
// Decoding an id costs a tree descent and a heap read, so the most recently decoded
// spellings sit in a direct-mapped cache of `cache_slots` entries (slot = id mod size). Ids are dense and a spelling never
// changes or moves, so an entry is never stale; ids that are consecutive never share a slot.
// Thread-safe. Inserts are serialized by one mutex, so a spelling is never interned twice.
// Lookups take no dictionary-wide lock: the trees and the heap latch their own pages, an id is
// published (the count rises) only after its record is complete, and the decode cache is guarded
// by 64 stripe locks, so readers that decode different ids do not wait on one another.
class DiskDictionary : public Dictionary {
 public:
  // `cache_slots` is the size of the decode cache described above; 0 turns it off.
  DiskDictionary(BufferPoolManager* bpm, Catalog* catalog, size_t cache_slots = kDefaultCacheSlots);
  static constexpr size_t kDefaultCacheSlots = 1 << 16;
  auto Insert(std::string_view term) -> term_id_t override;
  auto Lookup(std::string_view term) const -> std::optional<term_id_t> override;
  auto Lookup(term_id_t id) const -> std::optional<std::string> override;
  auto Size() const -> size_t override;

 private:
  auto LookupLocked(std::string_view term) const -> std::optional<term_id_t>;
  auto RidOf(term_id_t id) const -> std::optional<RID>;

  BufferPoolManager* bpm_;
  Catalog* catalog_;
  page_id_t header_page_{INVALID_PAGE_ID};
  size_t cache_slots_;
  struct CacheSlot {
    term_id_t id{0};
    std::string spelling;
  };
  static constexpr size_t kCacheStripes = 64;
  std::mutex insert_mu_;
  mutable std::array<std::mutex, kCacheStripes> cache_mu_;
  mutable std::vector<CacheSlot> cache_;
  std::unique_ptr<TermHeap> heap_;
  std::unique_ptr<BPlusTree> by_id_;
  std::unique_ptr<BPlusTree> by_hash_;
};

}  // namespace ontodb
