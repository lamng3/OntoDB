#include "dictionary/mem_dictionary.h"

namespace ontodb {

auto MemDictionary::Insert(std::string_view term) -> term_id_t {
  std::lock_guard<std::mutex> guard(mu_);
  const std::string key(term);
  if (const auto it = to_id_.find(key); it != to_id_.end()) {
    return it->second;
  }
  const term_id_t id = next_++;
  to_id_.emplace(key, id);
  to_term_.emplace(id, key);
  return id;
}

auto MemDictionary::Lookup(std::string_view term) const -> std::optional<term_id_t> {
  std::lock_guard<std::mutex> guard(mu_);
  const auto it = to_id_.find(std::string(term));
  if (it == to_id_.end()) {
    return std::nullopt;
  }
  return it->second;
}

auto MemDictionary::Lookup(term_id_t id) const -> std::optional<std::string> {
  std::lock_guard<std::mutex> guard(mu_);
  const auto it = to_term_.find(id);
  if (it == to_term_.end()) {
    return std::nullopt;
  }
  return it->second;
}

auto MemDictionary::Size() const -> size_t {
  std::lock_guard<std::mutex> guard(mu_);
  return to_term_.size();
}

}  // namespace ontodb
