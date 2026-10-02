#include "dictionary/disk_dictionary.h"

#include "common/exception.h"

namespace ontodb {

DiskDictionary::DiskDictionary(BufferPoolManager* bpm, Catalog* catalog)
    : bpm_(bpm), catalog_(catalog) {}

auto DiskDictionary::Insert(std::string_view) -> term_id_t {
  throw NotImplementedException("PLAN 3.2: DiskDictionary::Insert");
}
auto DiskDictionary::Lookup(std::string_view) const -> std::optional<term_id_t> {
  throw NotImplementedException("PLAN 3.2: DiskDictionary::Lookup");
}
auto DiskDictionary::Lookup(term_id_t) const -> std::optional<std::string> {
  throw NotImplementedException("PLAN 3.2: DiskDictionary::Lookup(id)");
}
auto DiskDictionary::Size() const -> size_t {
  throw NotImplementedException("PLAN 3.2: DiskDictionary::Size");
}

}  // namespace ontodb
