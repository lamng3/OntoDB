#pragma once

#include <mutex>
#include <string>
#include <unordered_map>

#include "dictionary/dictionary.h"

namespace ontodb {

// Process-local dictionary. A mutex makes shell sessions safe to share it.
// That mutex is not transactional isolation.
class MemDictionary : public Dictionary {
 public:
  auto Insert(std::string_view term) -> term_id_t override;
  auto Lookup(std::string_view term) const -> std::optional<term_id_t> override;
  auto Lookup(term_id_t id) const -> std::optional<std::string> override;
  auto Size() const -> size_t override;

 private:
  mutable std::mutex mu_;
  std::unordered_map<std::string, term_id_t> to_id_;
  std::unordered_map<term_id_t, std::string> to_term_;
  term_id_t next_{1};
};

}  // namespace ontodb
