#pragma once

#include "buffer/buffer_pool_manager.h"
#include "catalog/catalog.h"
#include "dictionary/dictionary.h"

namespace ontodb {

// Persistent dictionary. Both directions survive restart: spelling to id and
// id to spelling. Insert of an existing spelling returns the original id and
// does not allocate another. The on-disk representation is yours (PLAN 3.2);
// term bytes have a home in TermHeap (PLAN 3.1) if you want it.
class DiskDictionary : public Dictionary {
 public:
  DiskDictionary(BufferPoolManager* bpm, Catalog* catalog);
  auto Insert(std::string_view term) -> term_id_t override;
  auto Lookup(std::string_view term) const -> std::optional<term_id_t> override;
  auto Lookup(term_id_t id) const -> std::optional<std::string> override;
  auto Size() const -> size_t override;

 private:
  BufferPoolManager* bpm_;
  Catalog* catalog_;
};

}  // namespace ontodb
