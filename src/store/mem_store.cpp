#include "store/mem_store.h"

#include "store/mem_triple_iterator.h"

namespace ontodb {

auto MemStore::Insert(const Triple& triple) -> bool {
  std::lock_guard<std::mutex> guard(mu_);
  for (const auto& existing : triples_) {
    if (existing == triple) {
      return false;
    }
  }
  triples_.push_back(triple);
  return true;
}

auto MemStore::Delete(const Triple& triple) -> bool {
  std::lock_guard<std::mutex> guard(mu_);
  for (auto it = triples_.begin(); it != triples_.end(); ++it) {
    if (*it == triple) {
      triples_.erase(it);
      return true;
    }
  }
  return false;
}

auto MemStore::Scan(const TriplePattern& pattern) -> std::unique_ptr<TripleIterator> {
  std::lock_guard<std::mutex> guard(mu_);
  std::vector<Triple> matches;
  for (const auto& triple : triples_) {
    if (Matches(triple, pattern)) {
      matches.push_back(triple);
    }
  }
  return std::make_unique<MemTripleIterator>(std::move(matches));
}

auto MemStore::Size() const -> size_t {
  std::lock_guard<std::mutex> guard(mu_);
  return triples_.size();
}

}  // namespace ontodb
