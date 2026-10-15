#include "stats/statistics.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace ontodb {

namespace {

auto Find(const std::unordered_map<term_id_t, uint64_t>& counts, term_id_t id) -> uint64_t {
  const auto it = counts.find(id);
  return it == counts.end() ? 0 : it->second;
}

}  // namespace

auto Statistics::PairHash::operator()(const std::pair<term_id_t, term_id_t>& pair) const noexcept
    -> size_t {
  return std::hash<term_id_t>{}(pair.first) * 31 + std::hash<term_id_t>{}(pair.second);
}

auto Statistics::TripleHash::operator()(const Triple& t) const noexcept -> size_t {
  return (std::hash<term_id_t>{}(t.subject) * 31 + std::hash<term_id_t>{}(t.predicate)) * 31 +
         std::hash<term_id_t>{}(t.object);
}

void Statistics::Collect(TripleStore& store, const Dictionary&) {
  *this = Statistics{};
  auto scan = store.Scan(TriplePattern{});
  scan->Init();
  Triple t;
  while (scan->Next(&t)) {
    triples_++;
    by_s_[t.subject]++;
    by_p_[t.predicate]++;
    by_o_[t.object]++;
    by_sp_[{t.subject, t.predicate}]++;
    by_po_[{t.predicate, t.object}]++;
    by_so_[{t.subject, t.object}]++;
    all_.insert(t);
  }
}

auto Statistics::TripleCount() const -> uint64_t {
  return triples_;
}

auto Statistics::DistinctCount(std::string_view position) const -> uint64_t {
  if (position == "s") {
    return by_s_.size();
  }
  if (position == "p") {
    return by_p_.size();
  }
  if (position == "o") {
    return by_o_.size();
  }
  throw std::invalid_argument("position is s, p, or o, not " + std::string(position));
}

auto Statistics::PredicateCount(term_id_t predicate) const -> uint64_t {
  return Find(by_p_, predicate);
}

// Exact for every combination of bound positions.
auto Statistics::PatternCount(const TriplePattern& p) const -> uint64_t {
  auto pair = [](const PairCounts& counts, term_id_t a, term_id_t b) -> uint64_t {
    const auto it = counts.find({a, b});
    return it == counts.end() ? 0 : it->second;
  };
  const bool s = p.subject.has_value();
  const bool pr = p.predicate.has_value();
  const bool o = p.object.has_value();
  if (s && pr && o) {
    return all_.count(Triple{*p.subject, *p.predicate, *p.object});
  }
  if (s && pr) {
    return pair(by_sp_, *p.subject, *p.predicate);
  }
  if (pr && o) {
    return pair(by_po_, *p.predicate, *p.object);
  }
  if (s && o) {
    return pair(by_so_, *p.subject, *p.object);
  }
  if (s) {
    return Find(by_s_, *p.subject);
  }
  if (pr) {
    return Find(by_p_, *p.predicate);
  }
  if (o) {
    return Find(by_o_, *p.object);
  }
  return triples_;
}

}  // namespace ontodb
