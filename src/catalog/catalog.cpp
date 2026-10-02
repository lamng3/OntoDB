#include "catalog/catalog.h"

#include "common/exception.h"

namespace ontodb {

Catalog::Catalog(BufferPoolManager* bpm, page_id_t catalog_page_id)
    : bpm_(bpm), catalog_page_id_(catalog_page_id) {}

auto Catalog::GetIndexRoot(std::string_view) const -> page_id_t {
  throw NotImplementedException("PLAN 3.3: Catalog::GetIndexRoot");
}
void Catalog::SetIndexRoot(std::string_view, page_id_t) {
  throw NotImplementedException("PLAN 3.3: Catalog::SetIndexRoot");
}
auto Catalog::GetDictionaryRoot() const -> page_id_t {
  throw NotImplementedException("PLAN 3.3: Catalog::GetDictionaryRoot");
}
void Catalog::SetDictionaryRoot(page_id_t) {
  throw NotImplementedException("PLAN 3.3: Catalog::SetDictionaryRoot");
}
void Catalog::Flush() {
  throw NotImplementedException("PLAN 3.3: Catalog::Flush");
}

}  // namespace ontodb
