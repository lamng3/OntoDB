#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "common/exception.h"
#include "common/types.h"

namespace ontodb {

// Interns RDF terms. Insert of an existing spelling returns the same id.
// Ids are dense and start at 1. Lookup of an unknown spelling returns nullopt
// and does not insert.
//
// INVALID_TERM_ID (the largest id) is never assigned. Rows and bound patterns
// use it to mean "unbound", and TripleKey::PrefixUpper uses the all-0xFF key
// as the bound above everything. An implementation calls CheckAssignable
// before handing out a new id.
class Dictionary {
 public:
  virtual ~Dictionary() = default;

  virtual auto Insert(std::string_view term) -> term_id_t = 0;
  virtual auto Lookup(std::string_view term) const -> std::optional<term_id_t> = 0;
  virtual auto Lookup(term_id_t id) const -> std::optional<std::string> = 0;
  virtual auto Size() const -> size_t = 0;

  static void CheckAssignable(term_id_t id) {
    if (id == INVALID_TERM_ID) {
      throw StorageException("dictionary is full: the next id is reserved as INVALID_TERM_ID");
    }
  }
};

}  // namespace ontodb
