#include "semantic/type_closure.h"

#include <algorithm>
#include <set>

namespace ontodb {

namespace {

auto Drain(TripleStore* store, const TriplePattern& pattern) -> std::vector<Triple> {
  auto scan = store->Scan(pattern);
  scan->Init();
  std::vector<Triple> out;
  Triple triple;
  while (scan->Next(&triple)) {
    out.push_back(triple);
  }
  return out;
}

void SortUnique(std::vector<term_id_t>* v) {
  std::sort(v->begin(), v->end());
  v->erase(std::unique(v->begin(), v->end()), v->end());
}

}  // namespace

auto CollectSubClassEdges(TripleStore* store, term_id_t sub_class_of) -> std::vector<std::pair<term_id_t, term_id_t>> {
  std::vector<std::pair<term_id_t, term_id_t>> edges;
  for (const Triple& t : Drain(store, TriplePattern{std::nullopt, sub_class_of, std::nullopt})) {
    edges.emplace_back(t.subject, t.object);
  }
  return edges;
}

auto CollectTypeClasses(TripleStore* store, term_id_t rdf_type) -> std::vector<term_id_t> {
  std::vector<term_id_t> classes;
  for (const Triple& t : Drain(store, TriplePattern{std::nullopt, rdf_type, std::nullopt})) {
    classes.push_back(t.object);
  }
  SortUnique(&classes);
  return classes;
}

auto TypeLabelPairs(const ClassHierarchy& h, TripleStore* store, term_id_t rdf_type)
    -> std::vector<std::pair<uint32_t, term_id_t>> {
  std::vector<std::pair<uint32_t, term_id_t>> pairs;
  for (const Triple& t : Drain(store, TriplePattern{std::nullopt, rdf_type, std::nullopt})) {
    if (const uint32_t label = h.Label(t.object); label != 0) {
      pairs.emplace_back(label, t.subject);
    }
  }
  return pairs;
}

auto InstancesByIntervals(const ClassHierarchy& h, LabelIndex* index, term_id_t cls,
                          LabelIndex::ScanStats* stats) -> std::vector<term_id_t> {
  return index->Scan(h.Intervals(cls), stats);
}

auto InstancesByRewrite(const ClassHierarchy& h, TripleStore* store, term_id_t rdf_type, term_id_t cls,
                        size_t* scans) -> std::vector<term_id_t> {
  std::vector<term_id_t> out;
  // A class that is in no edge still has itself as instances.
  std::vector<term_id_t> below = h.Descendants(cls);
  if (below.empty()) {
    below.push_back(cls);
  }
  for (const term_id_t d : below) {
    if (scans != nullptr) {
      (*scans)++;
    }
    for (const Triple& t : Drain(store, TriplePattern{std::nullopt, rdf_type, d})) {
      out.push_back(t.subject);
    }
  }
  SortUnique(&out);
  return out;
}

auto MaterializeTypes(const ClassHierarchy& h, TripleStore* store, term_id_t rdf_type) -> size_t {
  std::vector<Triple> add;
  for (const Triple& t : Drain(store, TriplePattern{std::nullopt, rdf_type, std::nullopt})) {
    for (const term_id_t a : h.Ancestors(t.object)) {
      if (a != t.object) {
        add.push_back(Triple{t.subject, rdf_type, a});
      }
    }
  }
  std::sort(add.begin(), add.end());
  add.erase(std::unique(add.begin(), add.end()), add.end());
  return store->InsertBatch(add);
}

auto InstancesByScan(TripleStore* store, term_id_t rdf_type, term_id_t cls) -> std::vector<term_id_t> {
  std::vector<term_id_t> out;
  for (const Triple& t : Drain(store, TriplePattern{std::nullopt, rdf_type, cls})) {
    out.push_back(t.subject);
  }
  SortUnique(&out);
  return out;
}

}  // namespace ontodb
