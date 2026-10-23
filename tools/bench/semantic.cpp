// Semantic search measurements (benchmarks/semantic/). One program, five
// experiments, each printing a Markdown table to stdout:
//
//   order   <go.nt> <go.ovec>                 does any sort order put neighbors together?
//   labels  <work> <go.nt>                    how big are the interval labels, and what do they save?
//   types   <work> go <go.nt> <entities>      three ways to answer rdf:type C (synthetic instances)
//   types   <work> lubm <univ-bench.owl> <data.owl>...
//   knn     <go.nt> <go.ovec>                 recall against latency, for each vector index
//   hybrid  <go.nt> <go.ovec>                 nearest neighbors under a class, four strategies
//
// Methods and decision rules: benchmarks/semantic/question.md.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <queue>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <unistd.h>

#include "buffer/buffer_pool_manager.h"
#include "catalog/catalog.h"
#include "common/io_stats.h"
#include "dictionary/disk_dictionary.h"
#include "dictionary/mem_dictionary.h"
#include "loader/rdf_loader.h"
#include "semantic/class_hierarchy.h"
#include "semantic/flat_index.h"
#include "semantic/hnsw_index.h"
#include "semantic/hybrid_search.h"
#include "semantic/ivf_index.h"
#include "semantic/label_index.h"
#include "semantic/product_quantizer.h"
#include "semantic/type_closure.h"
#include "semantic/vector_store.h"
#include "store/indexed_store.h"
#include "store/indexed_triple_iterator.h"
#include "store/mem_store.h"

namespace fs = std::filesystem;
using namespace ontodb;  // NOLINT
using Clock = std::chrono::steady_clock;

namespace {

constexpr const char* kType = "<http://www.w3.org/1999/02/22-rdf-syntax-ns#type>";
constexpr const char* kSubClassOf = "<http://www.w3.org/2000/01/rdf-schema#subClassOf>";
constexpr const char* kLabel = "<http://www.w3.org/2000/01/rdf-schema#label>";
constexpr size_t kColdPool = 64;

auto Seconds(Clock::time_point since) -> double {
  return std::chrono::duration<double>(Clock::now() - since).count();
}

auto Mib(const fs::path& p) -> double {
  return static_cast<double>(fs::file_size(p)) / (1024.0 * 1024.0);
}

auto Percentile(std::vector<double> v, double p) -> double {
  if (v.empty()) {
    return 0;
  }
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())))];
}

auto Mean(const std::vector<double>& v) -> double {
  return v.empty() ? 0 : std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
}

// ---- an in-memory world: the ontology as triples, and its embeddings ----

struct World {
  MemDictionary dict;
  MemStore store;
  term_id_t type_{0}, sub_{0}, label_{0};

  void Load(const std::vector<fs::path>& files) {
    RdfLoader().LoadFiles(files, dict, store);
    type_ = dict.Insert(kType);
    sub_ = dict.Insert(kSubClassOf);
    label_ = dict.Insert(kLabel);
  }
};

auto Edges(World& w) -> std::vector<std::pair<term_id_t, term_id_t>> {
  return CollectSubClassEdges(&w.store, w.sub_);
}

// Ground truth for every query: the 10 nearest rows other than the query itself.
struct Truth {
  std::vector<uint32_t> queries;  // rows
  std::vector<std::vector<uint32_t>> nn;
};

auto MakeTruth(const VectorStore& vs, size_t count, uint64_t seed) -> Truth {
  FlatIndex exact(&vs, FlatIndex::Precision::kFloat32);
  std::mt19937_64 rng(seed);
  Truth t;
  for (size_t i = 0; i < count; i++) {
    t.queries.push_back(static_cast<uint32_t>(rng() % vs.Size()));
  }
  for (const uint32_t row : t.queries) {
    std::vector<uint32_t> nn;
    for (const auto& n : exact.Search(vs.Vector(row), 11)) {
      if (*vs.Row(n.id) != row && nn.size() < 10) {
        nn.push_back(*vs.Row(n.id));
      }
    }
    t.nn.push_back(std::move(nn));
  }
  return t;
}

auto RecallAt10(const VectorStore& vs, uint32_t query_row, const std::vector<Neighbor>& got, const std::vector<uint32_t>& truth) -> double {
  std::unordered_set<uint32_t> want(truth.begin(), truth.end());
  size_t hit = 0, seen = 0;
  for (const auto& n : got) {
    const uint32_t row = *vs.Row(n.id);
    if (row == query_row) {
      continue;
    }
    if (seen++ >= truth.size()) {
      break;
    }
    hit += want.count(row);
  }
  return truth.empty() ? 1.0 : static_cast<double>(hit) / static_cast<double>(truth.size());
}

// ---- experiment: order ----

auto RunOrder(int argc, char** argv) -> int {
  if (argc < 4) {
    std::cerr << "usage: order <go.nt> <go.ovec>\n";
    return 2;
  }
  World w;
  w.Load({argv[2]});
  VectorStore vs(384);
  const auto loaded = vs.Load(argv[3], &w.dict);
  const size_t n = vs.Size();
  std::cout << "<!-- " << loaded.stored << " vectors, " << w.store.Size() << " triples -->\n";
  const auto edges = Edges(w);
  const auto truth = MakeTruth(vs, 2000, 1);

  // Position of each row under each order.
  std::map<std::string, std::vector<uint32_t>> pos;
  auto from_order = [&](const std::vector<uint32_t>& order) {
    std::vector<uint32_t> p(n);
    for (uint32_t i = 0; i < n; i++) {
      p[order[i]] = i;
    }
    return p;
  };
  std::vector<uint32_t> rows(n);
  std::iota(rows.begin(), rows.end(), 0);
  {
    auto by_id = rows;
    std::sort(by_id.begin(), by_id.end(), [&](uint32_t a, uint32_t b) { return vs.IdAt(a) < vs.IdAt(b); });
    pos["dictionary id (first seen)"] = from_order(by_id);
  }
  for (const auto& [name, rule] : {std::pair{"hierarchy, deepest parent", ClassHierarchy::TreeParent::kDeepest},
                                   std::pair{"hierarchy, first parent", ClassHierarchy::TreeParent::kFirst}}) {
    const auto h = ClassHierarchy::Build(edges, rule);
    auto by_label = rows;
    std::sort(by_label.begin(), by_label.end(), [&](uint32_t a, uint32_t b) {
      return std::pair(h.Label(vs.IdAt(a)), vs.IdAt(a)) < std::pair(h.Label(vs.IdAt(b)), vs.IdAt(b));
    });
    pos[name] = from_order(by_label);
  }
  {
    std::unordered_map<term_id_t, std::string> names;
    auto scan = w.store.Scan(TriplePattern{std::nullopt, w.label_, std::nullopt});
    scan->Init();
    Triple t;
    while (scan->Next(&t)) {
      names[t.subject] = *w.dict.Lookup(t.object);
    }
    auto by_name = rows;
    std::sort(by_name.begin(), by_name.end(), [&](uint32_t a, uint32_t b) { return names[vs.IdAt(a)] < names[vs.IdAt(b)]; });
    pos["label, alphabetical"] = from_order(by_name);
  }
  {
    auto shuffled = rows;
    std::mt19937_64 rng(5);
    std::shuffle(shuffled.begin(), shuffled.end(), rng);
    pos["random"] = from_order(shuffled);
  }

  const std::vector<uint32_t> windows{10, 50, 100, 500, 1000, 5000};
  std::cout << "| Order | median rank gap to nearest neighbor |";
  for (const auto win : windows) {
    std::cout << " ±" << win << " |";
  }
  std::cout << "\n|---|---|";
  for (size_t i = 0; i < windows.size(); i++) {
    std::cout << "---|";
  }
  std::cout << "\n";
  for (const auto& [name, p] : pos) {
    std::vector<double> gap;
    std::vector<double> rec(windows.size(), 0.0);
    for (size_t q = 0; q < truth.queries.size(); q++) {
      const uint32_t qr = truth.queries[q];
      for (size_t j = 0; j < truth.nn[q].size(); j++) {
        const double d = std::fabs(static_cast<double>(p[truth.nn[q][j]]) - static_cast<double>(p[qr]));
        if (j == 0) {
          gap.push_back(d);
        }
        for (size_t wi = 0; wi < windows.size(); wi++) {
          rec[wi] += d <= windows[wi] ? 1.0 : 0.0;
        }
      }
    }
    std::printf("| %s | %.0f |", name.c_str(), Percentile(gap, 0.5));
    for (const double r : rec) {
      std::printf(" %.3f |", r / (static_cast<double>(truth.queries.size()) * 10.0));
    }
    std::printf("\n");
  }

  // How much of a term's meaning-neighborhood is within two steps in the hierarchy?
  std::vector<std::vector<uint32_t>> adj(n);
  for (const auto& [c, p] : edges) {
    const auto a = vs.Row(c), b = vs.Row(p);
    if (a && b && *a != *b) {
      adj[*a].push_back(*b);
      adj[*b].push_back(*a);
    }
  }
  double within2 = 0, size2 = 0, within1 = 0;
  for (size_t q = 0; q < truth.queries.size(); q++) {
    std::unordered_set<uint32_t> d1, d2;
    for (const uint32_t a : adj[truth.queries[q]]) {
      d1.insert(a);
      d2.insert(a);
      for (const uint32_t b : adj[a]) {
        d2.insert(b);
      }
    }
    size2 += static_cast<double>(d2.size());
    for (const uint32_t x : truth.nn[q]) {
      within1 += d1.count(x);
      within2 += d2.count(x);
    }
  }
  const double total = static_cast<double>(truth.queries.size()) * 10.0;
  std::printf("\n10 nearest neighbors by cosine, 2000 random terms: %.3f are parent or child, %.3f are within two is-a steps (a two-step neighborhood has %.0f terms on average, %.2f%% of all terms).\n",
              within1 / total, within2 / total, size2 / static_cast<double>(truth.queries.size()),
              100.0 * size2 / static_cast<double>(truth.queries.size()) / static_cast<double>(n));
  return 0;
}

// ---- cold sessions on an indexed store ----

struct Session {
  IoStats stats;
  DiskManager disk;
  BufferPoolManager pool;
  Catalog catalog;
  DiskDictionary dict;
  IndexedStore store;

  Session(const fs::path& file, size_t frames)
      : disk(file.string(), &stats), pool(frames, &disk, 2, nullptr, &stats), catalog(&pool), dict(&pool, &catalog), store(&catalog, &pool, &dict) {}
};

// Loads files into a fresh indexed store file; returns seconds.
auto BuildStore(const fs::path& file, const std::vector<fs::path>& files, uint64_t* triples) -> double {
  fs::remove(file);
  const auto started = Clock::now();
  DiskManager disk(file.string());
  BufferPoolManager pool(4096, &disk, 2);
  Catalog catalog(&pool);
  DiskDictionary dict(&pool, &catalog);
  IndexedStore store(&catalog, &pool, &dict);
  *triples = RdfLoader().LoadFiles(files, dict, store).triples;
  pool.FlushAllPages();
  disk.ShutDown();
  return Seconds(started);
}

auto Bucket(size_t count) -> int {
  return count <= 1 ? 0 : count <= 9 ? 1 : count <= 99 ? 2 : count <= 999 ? 3 : 4;
}
constexpr const char* kBucketName[] = {"1", "2-9", "10-99", "100-999", "1000+"};

// ---- experiment: labels ----

auto RunLabels(int argc, char** argv) -> int {
  if (argc < 4) {
    std::cerr << "usage: labels <work dir> <go.nt>\n";
    return 2;
  }
  const fs::path work = argv[2];
  fs::create_directories(work);
  World w;
  w.Load({argv[3]});
  const auto edges = Edges(w);

  std::cout << "| Spanning-forest rule | classes | edges | non-tree edges | intervals | per class: mean / p50 / p90 / p99 / max | closure pairs | labels (KiB) | closure (MiB) | build (ms) |\n|---|---|---|---|---|---|---|---|---|---|\n";
  for (const auto& [name, rule] : {std::pair{"deepest parent", ClassHierarchy::TreeParent::kDeepest},
                                   std::pair{"first parent", ClassHierarchy::TreeParent::kFirst}}) {
    const auto started = Clock::now();
    const auto h = ClassHierarchy::Build(edges, rule);
    const double ms = Seconds(started) * 1000.0;
    const auto& st = h.GetStats();
    std::vector<double> counts;
    for (const term_id_t c : [&] { std::vector<term_id_t> ids; for (const auto& e : edges) { ids.push_back(e.first); ids.push_back(e.second); } std::sort(ids.begin(), ids.end()); ids.erase(std::unique(ids.begin(), ids.end()), ids.end()); return ids; }()) {
      counts.push_back(static_cast<double>(h.Intervals(c).size()));
    }
    std::printf("| %s | %zu | %zu | %zu | %zu | %.2f / %.0f / %.0f / %.0f / %zu | %zu | %.0f | %.1f | %.1f |\n", name, st.classes, st.edges,
                st.non_tree_edges, st.intervals, Mean(counts), Percentile(counts, 0.5), Percentile(counts, 0.9),
                Percentile(counts, 0.99), st.max_intervals, st.closure_pairs, static_cast<double>(st.intervals) * 8 / 1024.0,
                static_cast<double>(st.closure_pairs) * 24 / (1024.0 * 1024.0), ms);
    if (st.max_intervals > 1 + st.non_tree_edges) {
      std::cout << "BOUND VIOLATED\n";
    }
  }

  // Reachability: is d at or below c? Intervals against a search up the hierarchy.
  const auto h = ClassHierarchy::Build(edges);
  std::vector<term_id_t> ids;
  for (const auto& e : edges) {
    ids.push_back(e.first);
  }
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  std::mt19937_64 rng(2);
  std::vector<std::pair<term_id_t, term_id_t>> pairs;
  for (int i = 0; i < 200000; i++) {
    pairs.emplace_back(ids[rng() % ids.size()], ids[rng() % ids.size()]);
  }
  size_t yes = 0;
  auto t0 = Clock::now();
  for (const auto& [d, c] : pairs) {
    yes += h.IsDescendant(d, c) ? 1 : 0;
  }
  const double interval_ns = Seconds(t0) * 1e9 / static_cast<double>(pairs.size());
  std::unordered_map<term_id_t, std::vector<term_id_t>> parents;
  for (const auto& [c, p] : edges) {
    parents[c].push_back(p);
  }
  size_t yes2 = 0;
  t0 = Clock::now();
  const size_t search_n = 20000;
  for (size_t i = 0; i < search_n; i++) {
    const auto& [d, c] = pairs[i];
    std::vector<term_id_t> todo{d};
    std::unordered_set<term_id_t> seen{d};
    bool found = d == c;
    while (!found && !todo.empty()) {
      const term_id_t v = todo.back();
      todo.pop_back();
      for (const term_id_t p : parents[v]) {
        if (p == c) {
          found = true;
          break;
        }
        if (seen.insert(p).second) {
          todo.push_back(p);
        }
      }
    }
    yes2 += found ? 1 : 0;
  }
  const double search_ns = Seconds(t0) * 1e9 / static_cast<double>(search_n);
  size_t agree = 0;
  for (size_t i = 0; i < search_n; i++) {
    agree += h.IsDescendant(pairs[i].first, pairs[i].second) ? 1 : 0;
  }
  std::printf("\nIs d at or below c, over random class pairs: %.0f ns with intervals, %.0f ns by searching up the parents (%zu of %zu pairs true; the two methods agree: %s).\n",
              interval_ns, search_ns, yes, pairs.size(), agree == yes2 ? "yes" : "NO");

  // Descendants(C) from the database: one range scan per interval, against a
  // breadth-first search with one POS scan per class.
  const fs::path file = work / "go.db";
  uint64_t triples = 0;
  const double load_s = BuildStore(file, {argv[3]}, &triples);
  page_id_t header = INVALID_PAGE_ID;
  double index_ms = 0;
  size_t label_pairs = 0;
  {
    Session s(file, 4096);
    const auto sub = *s.dict.Lookup(kSubClassOf);
    const auto dedges = CollectSubClassEdges(&s.store, sub);
    const auto started = Clock::now();
    const auto dh = ClassHierarchy::Build(dedges);
    std::vector<std::pair<uint32_t, term_id_t>> pr;
    for (const auto& e : dedges) {
      pr.emplace_back(dh.Label(e.first), e.first);
      pr.emplace_back(dh.Label(e.second), e.second);
    }
    LabelIndex idx(&s.pool);
    idx.BulkLoad(pr);
    index_ms = Seconds(started) * 1000.0;
    label_pairs = pr.size();
    header = idx.Header();
    s.pool.FlushAllPages();
    s.disk.ShutDown();
  }
  std::printf("\nLoaded %llu triples in %.2f s (%.1f MiB). Building the labels and their B+ tree from the stored triples: %.0f ms.\n",
              static_cast<unsigned long long>(triples), load_s, Mib(file), index_ms);
  (void)label_pairs;

  // Sample classes by subtree size.
  std::map<int, std::vector<term_id_t>> sample;
  {
    Session s(file, 4096);
    const auto sub = *s.dict.Lookup(kSubClassOf);
    const auto dh = ClassHierarchy::Build(CollectSubClassEdges(&s.store, sub));
    std::vector<term_id_t> all;
    for (const auto& e : CollectSubClassEdges(&s.store, sub)) {
      all.push_back(e.second);
      all.push_back(e.first);
    }
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    std::mt19937_64 r(3);
    std::shuffle(all.begin(), all.end(), r);
    for (const term_id_t c : all) {
      auto& v = sample[Bucket(dh.DescendantCount(c))];
      if (v.size() < 40) {
        v.push_back(c);
      }
    }
  }
  std::cout << "\n| Classes below C | method | answers | range scans | keys read | page reads | µs |\n|---|---|---|---|---|---|---|\n";
  for (const auto& [bucket, classes] : sample) {
    struct Acc {
      std::vector<double> answers, scans, keys, reads, us;
    } iv, rw;
    for (const term_id_t c : classes) {
      {  // intervals
        Session s(file, kColdPool);
        const auto sub = *s.dict.Lookup(kSubClassOf);
        const auto dh = ClassHierarchy::Build(CollectSubClassEdges(&s.store, sub));
        LabelIndex idx(&s.pool, header);
        s.stats.Reset();
        const auto t = Clock::now();
        LabelIndex::ScanStats st;
        const auto got = idx.Scan(dh.Intervals(c), &st);
        iv.us.push_back(Seconds(t) * 1e6);
        iv.answers.push_back(static_cast<double>(got.size()));
        iv.scans.push_back(static_cast<double>(st.scans));
        iv.keys.push_back(static_cast<double>(st.keys));
        iv.reads.push_back(static_cast<double>(s.stats.page_reads()));
      }
      {  // rewrite: breadth-first search down the hierarchy, one POS scan per class
        Session s(file, kColdPool);
        const auto sub = *s.dict.Lookup(kSubClassOf);
        s.stats.Reset();
        const auto t = Clock::now();
        std::set<term_id_t> seen{c};
        std::vector<term_id_t> todo{c};
        size_t scans = 0, keys = 0;
        while (!todo.empty()) {
          const term_id_t v = todo.back();
          todo.pop_back();
          auto scan = s.store.Scan(TriplePattern{std::nullopt, sub, v});
          scans++;
          Triple tr;
          while (scan->Next(&tr)) {
            if (seen.insert(tr.subject).second) {
              todo.push_back(tr.subject);
            }
          }
          keys += dynamic_cast<IndexedTripleIterator&>(*scan).KeysExamined();
        }
        rw.us.push_back(Seconds(t) * 1e6);
        rw.answers.push_back(static_cast<double>(seen.size()));
        rw.scans.push_back(static_cast<double>(scans));
        rw.keys.push_back(static_cast<double>(keys));
        rw.reads.push_back(static_cast<double>(s.stats.page_reads()));
      }
    }
    std::printf("| %s | intervals | %.1f | %.2f | %.1f | %.1f | %.0f |\n", kBucketName[bucket], Mean(iv.answers), Mean(iv.scans), Mean(iv.keys), Mean(iv.reads), Mean(iv.us));
    std::printf("| %s | one scan per class | %.1f | %.1f | %.1f | %.1f | %.0f |\n", kBucketName[bucket], Mean(rw.answers), Mean(rw.scans), Mean(rw.keys), Mean(rw.reads), Mean(rw.us));
  }
  return 0;
}

// ---- experiment: types ----

auto WriteNt(const fs::path& path, const std::vector<std::string>& lines) -> void {
  std::ofstream out(path);
  for (const auto& l : lines) {
    out << l << " .\n";
  }
}

auto RunTypes(int argc, char** argv) -> int {
  if (argc < 5) {
    std::cerr << "usage: types <work> go <go.nt> <entities> | types <work> lubm <ontology> <data>...\n";
    return 2;
  }
  const fs::path work = argv[2];
  fs::create_directories(work);
  const std::string kind = argv[3];
  std::vector<fs::path> base;
  World w;
  if (kind == "go") {
    base = {argv[4]};
    w.Load(base);
    const size_t entities = std::stoul(argv[5]);
    const auto edges = Edges(w);
    std::vector<term_id_t> classes;
    for (const auto& e : edges) {
      classes.push_back(e.first);
    }
    std::sort(classes.begin(), classes.end());
    classes.erase(std::unique(classes.begin(), classes.end()), classes.end());
    std::mt19937_64 rng(4);
    std::vector<std::string> lines;
    for (size_t e = 0; e < entities; e++) {
      const size_t types = 1 + rng() % 3;
      std::set<term_id_t> chosen;
      for (size_t k = 0; k < types; k++) {
        chosen.insert(classes[rng() % classes.size()]);
      }
      for (const term_id_t c : chosen) {
        lines.push_back("<urn:e/" + std::to_string(e) + "> " + kType + " " + *w.dict.Lookup(c));
      }
    }
    WriteNt(work / "entities.nt", lines);
    base.push_back(work / "entities.nt");
    w.Load({work / "entities.nt"});  // the entities join the ontology already loaded
  } else {
    for (int i = 4; i < argc; i++) {
      base.push_back(argv[i]);
    }
    w.Load(base);
  }
  // The inferred type triples, as N-Triples, so the materialized store bulk-loads too.
  const auto edges = Edges(w);
  const auto h = ClassHierarchy::Build(edges, ClassHierarchy::TreeParent::kDeepest, CollectTypeClasses(&w.store, w.type_));
  std::vector<std::string> inferred;
  {
    std::set<std::pair<term_id_t, term_id_t>> have;  // direct (instance, class) pairs and those already inferred
    auto scan = w.store.Scan(TriplePattern{std::nullopt, w.type_, std::nullopt});
    scan->Init();
    std::vector<Triple> direct;
    Triple t;
    while (scan->Next(&t)) {
      direct.push_back(t);
      have.insert({t.subject, t.object});
    }
    for (const Triple& d : direct) {
      for (const term_id_t a : h.Ancestors(d.object)) {
        if (have.insert({d.subject, a}).second) {
          inferred.push_back(*w.dict.Lookup(d.subject) + " " + kType + " " + *w.dict.Lookup(a));
        }
      }
    }
  }
  WriteNt(work / "inferred.nt", inferred);
  auto closed = base;
  closed.push_back(work / "inferred.nt");

  uint64_t t_base = 0, t_closed = 0;
  const double load_base = BuildStore(work / "base.db", base, &t_base);
  const double load_closed = BuildStore(work / "closed.db", closed, &t_closed);
  page_id_t header = INVALID_PAGE_ID;
  double label_ms = 0;
  const double base_mib = Mib(work / "base.db");
  {
    Session s(work / "base.db", 4096);
    const auto type = *s.dict.Lookup(kType);
    const auto sub = *s.dict.Lookup(kSubClassOf);
    const auto started = Clock::now();
    const auto dh = ClassHierarchy::Build(CollectSubClassEdges(&s.store, sub), ClassHierarchy::TreeParent::kDeepest, CollectTypeClasses(&s.store, type));
    LabelIndex idx(&s.pool);
    idx.BulkLoad(TypeLabelPairs(dh, &s.store, type));
    label_ms = Seconds(started) * 1000.0;
    header = idx.Header();
    s.pool.FlushAllPages();
    s.disk.ShutDown();
  }
  const double with_labels_mib = Mib(work / "base.db");
  std::cout << "| Strategy | what is stored | triples | file (MiB) | load / index (s) |\n|---|---|---|---|---|\n";
  std::printf("| rewrite | the data | %llu | %.1f | %.2f |\n", static_cast<unsigned long long>(t_base), base_mib, load_base);
  std::printf("| intervals | the data + one key per type triple | %llu | %.1f | %.2f + %.2f |\n", static_cast<unsigned long long>(t_base), with_labels_mib, load_base, label_ms / 1000.0);
  std::printf("| materialize | the data + inferred types | %llu | %.1f | %.2f |\n\n", static_cast<unsigned long long>(t_closed), Mib(work / "closed.db"), load_closed);

  // Query classes: those with instances, by subtree size.
  std::map<int, std::vector<std::string>> sample;
  {
    Session s(work / "base.db", 4096);
    const auto type = *s.dict.Lookup(kType);
    const auto sub = *s.dict.Lookup(kSubClassOf);
    const auto dh = ClassHierarchy::Build(CollectSubClassEdges(&s.store, sub), ClassHierarchy::TreeParent::kDeepest, CollectTypeClasses(&s.store, type));
    std::vector<term_id_t> all = CollectTypeClasses(&s.store, type);
    for (const auto& e : CollectSubClassEdges(&s.store, sub)) {
      all.push_back(e.second);
    }
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    std::mt19937_64 r(6);
    std::shuffle(all.begin(), all.end(), r);
    for (const term_id_t c : all) {
      auto& v = sample[Bucket(dh.DescendantCount(c))];
      if (v.size() < 30) {
        v.push_back(*s.dict.Lookup(c));
      }
    }
  }
  std::cout << "| Classes below C | strategy | answers | scans | keys read | page reads | µs |\n|---|---|---|---|---|---|---|\n";
  // The sampled classes, with the interval strategy's answer count and warm time (a pool that
  // holds the whole file, median of 9 runs, as ids and decoded to strings), for benchmarks/compare to run against other engines.
  std::ofstream classes_out(work / "classes.tsv");
  for (const auto& [bucket, spellings] : sample) {
    struct Acc {
      std::vector<double> answers, scans, keys, reads, us;
    } iv, rw, mt;
    bool agree = true;
    for (const auto& spelling : spellings) {
      std::vector<term_id_t> a_iv, a_rw, a_mt;
      std::vector<std::string> names_iv, names_rw, names_mt;
      {
        Session s(work / "base.db", 4096);
        const auto type = *s.dict.Lookup(kType);
        const auto sub = *s.dict.Lookup(kSubClassOf);
        const auto c = *s.dict.Lookup(spelling);
        const auto dh = ClassHierarchy::Build(CollectSubClassEdges(&s.store, sub), ClassHierarchy::TreeParent::kDeepest, CollectTypeClasses(&s.store, type));
        LabelIndex idx(&s.pool, header);
        std::vector<double> id_runs, string_runs;
        size_t answers = 0;
        for (int r = 0; r < 10; r++) {  // the first run warms the pool
          const auto t = Clock::now();
          const auto ids = InstancesByIntervals(dh, &idx, c, nullptr);
          const double id_us = Seconds(t) * 1e6;
          size_t bytes = 0;
          for (const term_id_t id : ids) {  // decoded to spellings, as another engine returns them
            bytes += s.dict.Lookup(id)->size();
          }
          volatile size_t sink = bytes;  // keeps the decoding from being optimized away
          (void)sink;
          answers = ids.size();
          id_runs.push_back(id_us);
          string_runs.push_back(Seconds(t) * 1e6);
        }
        id_runs.erase(id_runs.begin());
        string_runs.erase(string_runs.begin());
        std::sort(id_runs.begin(), id_runs.end());
        std::sort(string_runs.begin(), string_runs.end());
        classes_out << kBucketName[bucket] << '\t' << spelling << '\t' << answers << '\t' << id_runs[id_runs.size() / 2] << '\t' << string_runs[string_runs.size() / 2] << '\n';
      }
      {
        Session s(work / "base.db", kColdPool);
        const auto type = *s.dict.Lookup(kType);
        const auto sub = *s.dict.Lookup(kSubClassOf);
        const auto c = *s.dict.Lookup(spelling);
        const auto dh = ClassHierarchy::Build(CollectSubClassEdges(&s.store, sub), ClassHierarchy::TreeParent::kDeepest, CollectTypeClasses(&s.store, type));
        LabelIndex idx(&s.pool, header);
        s.stats.Reset();
        auto t = Clock::now();
        LabelIndex::ScanStats st;
        a_iv = InstancesByIntervals(dh, &idx, c, &st);
        iv.us.push_back(Seconds(t) * 1e6);
        iv.answers.push_back(static_cast<double>(a_iv.size()));
        iv.scans.push_back(static_cast<double>(st.scans));
        iv.keys.push_back(static_cast<double>(st.keys));
        iv.reads.push_back(static_cast<double>(s.stats.page_reads()));
        for (const auto id : a_iv) {
          names_iv.push_back(*s.dict.Lookup(id));
        }
        // The hierarchy is derived from the store here, outside the timed part; a
        // running system keeps it. The rewrite is timed on its own.
        s.stats.Reset();
        t = Clock::now();
        size_t scans = 0, keys = 0;
        std::set<term_id_t> out;
        for (const term_id_t d : dh.Descendants(c).empty() ? std::vector<term_id_t>{c} : dh.Descendants(c)) {
          auto scan = s.store.Scan(TriplePattern{std::nullopt, type, d});
          scans++;
          Triple tr;
          while (scan->Next(&tr)) {
            out.insert(tr.subject);
          }
          keys += dynamic_cast<IndexedTripleIterator&>(*scan).KeysExamined();
        }
        rw.us.push_back(Seconds(t) * 1e6);
        rw.answers.push_back(static_cast<double>(out.size()));
        rw.scans.push_back(static_cast<double>(scans));
        rw.keys.push_back(static_cast<double>(keys));
        rw.reads.push_back(static_cast<double>(s.stats.page_reads()));
        for (const auto id : out) {
          names_rw.push_back(*s.dict.Lookup(id));
        }
      }
      {
        Session s(work / "closed.db", kColdPool);
        const auto type = *s.dict.Lookup(kType);
        const auto c = *s.dict.Lookup(spelling);
        s.stats.Reset();
        const auto t = Clock::now();
        auto scan = s.store.Scan(TriplePattern{std::nullopt, type, c});
        std::set<term_id_t> out;
        Triple tr;
        while (scan->Next(&tr)) {
          out.insert(tr.subject);
        }
        mt.us.push_back(Seconds(t) * 1e6);
        mt.answers.push_back(static_cast<double>(out.size()));
        mt.scans.push_back(1);
        mt.keys.push_back(static_cast<double>(dynamic_cast<IndexedTripleIterator&>(*scan).KeysExamined()));
        mt.reads.push_back(static_cast<double>(s.stats.page_reads()));
        for (const auto id : out) {
          names_mt.push_back(*s.dict.Lookup(id));
        }
      }
      std::sort(names_iv.begin(), names_iv.end());
      std::sort(names_rw.begin(), names_rw.end());
      std::sort(names_mt.begin(), names_mt.end());
      agree = agree && names_iv == names_rw && names_rw == names_mt;
    }
    for (const auto& [name, acc] : {std::pair<const char*, const Acc*>{"intervals", &iv}, {"rewrite", &rw}, {"materialized", &mt}}) {
      std::printf("| %s | %s | %.1f | %.1f | %.0f | %.1f | %.0f |\n", kBucketName[bucket], name, Mean(acc->answers), Mean(acc->scans), Mean(acc->keys), Mean(acc->reads), Mean(acc->us));
    }
    if (!agree) {
      std::printf("ANSWERS DISAGREE in bucket %s\n", kBucketName[bucket]);
    }
  }
  return 0;
}

// ---- flat PQ, for the comparison only ----

struct PqFlat {
  const VectorStore* vs;
  ProductQuantizer pq;
  std::vector<uint8_t> codes;
  PqFlat(const VectorStore* store, uint32_t m) : vs(store), pq(store->Dim(), m) {
    pq.Train(vs->Vector(0), std::min<size_t>(vs->Size(), 20000), 10, 1);
    codes.resize(vs->Size() * m);
    for (uint32_t row = 0; row < vs->Size(); row++) {
      pq.Encode(vs->Vector(row), codes.data() + static_cast<size_t>(row) * m);
    }
  }
  // The best `rerank` rows by ADC, re-scored exactly when rerank > k.
  auto Search(const float* q, size_t k, size_t rerank) const -> std::vector<Neighbor> {
    const auto table = pq.Table(q);
    using E = std::pair<float, uint32_t>;
    std::priority_queue<E, std::vector<E>, std::greater<>> heap;
    const size_t keep = std::max(k, rerank);
    for (uint32_t row = 0; row < vs->Size(); row++) {
      const float s = ProductQuantizer::Score(table, codes.data() + static_cast<size_t>(row) * pq.M(), pq.M());
      if (heap.size() < keep) {
        heap.push({s, row});
      } else if (s > heap.top().first) {
        heap.pop();
        heap.push({s, row});
      }
    }
    std::vector<Neighbor> out;
    while (!heap.empty()) {
      const uint32_t row = heap.top().second;
      float s = heap.top().first;
      if (rerank > k) {
        s = 0;
        for (uint32_t i = 0; i < vs->Dim(); i++) {
          s += q[i] * vs->Vector(row)[i];
        }
      }
      out.push_back({vs->IdAt(row), s});
      heap.pop();
    }
    std::sort(out.begin(), out.end(), [](const Neighbor& a, const Neighbor& b) { return a.score > b.score; });
    out.resize(std::min(out.size(), k));
    return out;
  }
  auto Bytes() const -> size_t { return codes.size(); }
};

struct Row {
  std::string name, param;
  double recall, us, scored;
};

// ---- experiment: knn ----

template <typename Search>
auto Measure(const VectorStore& vs, const Truth& truth, const Search& search, size_t* scored) -> std::pair<double, double> {
  // One warm pass, then a timed pass over all queries.
  for (size_t i = 0; i < std::min<size_t>(50, truth.queries.size()); i++) {
    (void)search(vs.Vector(truth.queries[i]));
  }
  double recall = 0;
  const auto t = Clock::now();
  std::vector<std::vector<Neighbor>> out;
  out.reserve(truth.queries.size());
  for (const uint32_t row : truth.queries) {
    out.push_back(search(vs.Vector(row)));
  }
  const double us = Seconds(t) * 1e6 / static_cast<double>(truth.queries.size());
  for (size_t i = 0; i < truth.queries.size(); i++) {
    recall += RecallAt10(vs, truth.queries[i], out[i], truth.nn[i]);
  }
  (void)scored;
  return {recall / static_cast<double>(truth.queries.size()), us};
}

auto RunKnn(int argc, char** argv) -> int {
  if (argc < 4) {
    std::cerr << "usage: knn <go.nt> <go.ovec>\n";
    return 2;
  }
  World w;
  w.Load({argv[2]});
  VectorStore vs(384);
  vs.Load(argv[3], &w.dict);
  std::cout << "<!-- " << vs.Size() << " vectors of " << vs.Dim() << " floats -->\n";
  const auto truth = MakeTruth(vs, 1000, 1);
  std::vector<std::string> lines;
  auto row = [&](const std::string& name, const std::string& param, double recall, double us, double mem_mib, double build_s) {
    char buf[256];
    std::snprintf(buf, sizeof buf, "| %s | %s | %.3f | %.0f | %.1f | %.1f |", name.c_str(), param.c_str(), recall, us, mem_mib, build_s);
    std::cout << buf << "\n";
  };
  std::cout << "| Index | setting | recall@10 | µs / query | memory (MiB) | build (s) |\n|---|---|---|---|---|---|\n";

  const auto q_scored = [](size_t*) {};
  (void)q_scored;
  size_t sink = 0;
  {
    FlatIndex flat(&vs, FlatIndex::Precision::kFloat32);
    const auto [r, us] = Measure(vs, truth, [&](const float* q) { return flat.Search(q, 11); }, &sink);
    row("flat float32", "exact", r, us, static_cast<double>(flat.Bytes()) / 1048576.0, 0);
  }
  {
    const auto t = Clock::now();
    FlatIndex flat(&vs, FlatIndex::Precision::kSq8);
    const double build = Seconds(t);
    const auto [r, us] = Measure(vs, truth, [&](const float* q) { return flat.Search(q, 11); }, &sink);
    row("flat sq8", "exact over codes", r, us, static_cast<double>(flat.Bytes()) / 1048576.0, build);
  }
  for (const uint32_t m : {48u, 96u}) {
    const auto t = Clock::now();
    PqFlat pq(&vs, m);
    const double build = Seconds(t);
    for (const size_t rerank : {size_t{0}, size_t{100}}) {
      const auto [r, us] = Measure(vs, truth, [&](const float* q) { return pq.Search(q, 11, rerank); }, &sink);
      row("flat pq m=" + std::to_string(m), rerank == 0 ? "codes only" : "rerank 100 exactly", r, us, static_cast<double>(pq.Bytes()) / 1048576.0, build);
    }
  }
  fs::path tmp = fs::temp_directory_path() / ("ontodb-knn-" + std::to_string(::getpid()) + ".db");
  fs::remove(tmp);
  DiskManager disk(tmp.string());
  BufferPoolManager pool(8192, &disk, 2);
  for (const uint32_t lists : {128u, 512u, 2048u}) {
    for (const auto enc : {IvfIndex::Encoding::kFloat32, IvfIndex::Encoding::kSq8, IvfIndex::Encoding::kPq}) {
      if (lists != 512 && enc != IvfIndex::Encoding::kFloat32) {
        continue;
      }
      const auto t = Clock::now();
      IvfIndex ivf(&vs, &pool, {.lists = lists, .encoding = enc, .pq_m = 96});
      ivf.Build();
      const double build = Seconds(t);
      const char* ename = enc == IvfIndex::Encoding::kFloat32 ? "float32" : enc == IvfIndex::Encoding::kSq8 ? "sq8" : "pq96";
      for (const uint32_t nprobe : {1u, 2u, 4u, 8u, 16u, 32u, 64u}) {
        if (nprobe > lists) {
          continue;
        }
        const auto [r, us] = Measure(vs, truth, [&](const float* q) { return ivf.Search(q, 11, nprobe); }, &sink);
        row("ivf L=" + std::to_string(lists) + " " + ename, "nprobe " + std::to_string(nprobe), r, us, static_cast<double>(ivf.Bytes()) / 1048576.0, build);
      }
    }
  }
  for (const uint32_t m : {8u, 16u, 32u}) {
    const auto t = Clock::now();
    HnswIndex hnsw(&vs, {.m = m, .ef_construction = 200});
    hnsw.Build();
    const double build = Seconds(t);
    const double mem = (static_cast<double>(hnsw.Bytes()) + static_cast<double>(vs.Size() * vs.Dim() * sizeof(float))) / 1048576.0;
    for (const uint32_t ef : {10u, 20u, 40u, 80u, 160u, 320u}) {
      if (m != 16 && ef != 40 && ef != 160) {
        continue;
      }
      const auto [r, us] = Measure(vs, truth, [&](const float* q) { return hnsw.Search(q, 11, ef); }, &sink);
      row("hnsw M=" + std::to_string(m), "ef " + std::to_string(ef), r, us, mem, build);
    }
  }
  fs::remove(tmp);
  return 0;
}

// ---- experiment: hybrid ----

auto RunHybrid(int argc, char** argv) -> int {
  if (argc < 4) {
    std::cerr << "usage: hybrid <go.nt> <go.ovec>\n";
    return 2;
  }
  World w;
  w.Load({argv[2]});
  VectorStore vs(384);
  vs.Load(argv[3], &w.dict);
  const auto edges = Edges(w);
  const auto h = ClassHierarchy::Build(edges);
  fs::path tmp = fs::temp_directory_path() / ("ontodb-hybrid-" + std::to_string(::getpid()) + ".db");
  fs::remove(tmp);
  DiskManager disk(tmp.string());
  BufferPoolManager pool(8192, &disk, 2);
  LabelIndex classes(&pool);
  {
    std::vector<std::pair<uint32_t, term_id_t>> pr;
    for (uint32_t row = 0; row < vs.Size(); row++) {
      if (h.Label(vs.IdAt(row)) != 0) {
        pr.emplace_back(h.Label(vs.IdAt(row)), vs.IdAt(row));
      }
    }
    classes.BulkLoad(pr);
  }
  FlatIndex flat(&vs, FlatIndex::Precision::kFloat32);
  IvfIndex ivf(&vs, &pool, {.lists = 512});
  ivf.Build();
  HnswIndex hnsw(&vs, {.m = 16, .ef_construction = 200});
  hnsw.Build();
  HybridSearch search({&h, &classes, &vs, &flat, &ivf, &hnsw});
  const HybridSearch::Params params{.nprobe = 16, .ef = 64, .pre_threshold = 4096, .max_visits = 0};
  std::cout << "<!-- " << vs.Size() << " vectors; IVF L=512 nprobe=16, HNSW M=16 ef=64 -->\n";

  // Classes by subtree size.
  std::vector<term_id_t> all;
  for (const auto& e : edges) {
    all.push_back(e.second);
  }
  std::sort(all.begin(), all.end());
  all.erase(std::unique(all.begin(), all.end()), all.end());
  auto size_bucket = [](size_t s) { return s <= 10 ? 0 : s <= 100 ? 1 : s <= 1000 ? 2 : s <= 5000 ? 3 : 4; };
  const char* names[] = {"<=10", "11-100", "101-1000", "1001-5000", ">5000"};
  std::mt19937_64 rng(7);
  std::shuffle(all.begin(), all.end(), rng);
  std::map<int, std::vector<term_id_t>> sample;
  for (const term_id_t c : all) {
    if (!vs.Row(c).has_value()) {
      continue;
    }
    auto& v = sample[size_bucket(h.DescendantCount(c))];
    if (v.size() < 20) {
      v.push_back(c);
    }
  }
  std::cout << "| Classes below C | query | strategy | recall@10 | µs / query | vectors scored | class members resolved | index searches |\n|---|---|---|---|---|---|---|---|\n";
  using S = HybridSearch::Strategy;
  const S strategies[] = {S::kPreFilter, S::kPostFilterHnsw, S::kPostFilterIvf, S::kMaskHnsw, S::kMaskIvf, S::kAuto};
  for (const auto& [bucket, classes_in_bucket] : sample) {
    for (const bool inside : {true, false}) {
      struct Acc {
        std::vector<double> recall, us, scored, members, rounds;
      };
      std::map<S, Acc> acc;
      double inside_share = 0;
      int queries = 0;
      for (const term_id_t c : classes_in_bucket) {
        const auto below = h.Descendants(c);
        const std::unordered_set<term_id_t> members(below.begin(), below.end());
        std::vector<uint32_t> member_rows;
        for (const term_id_t id : below) {
          if (const auto r = vs.Row(id)) {
            member_rows.push_back(*r);
          }
        }
        // The query vector: a random member (inside), or a random term that is not below C.
        std::mt19937_64 qr(c);
        uint32_t qrow = 0;
        if (inside) {
          qrow = member_rows[qr() % member_rows.size()];
        } else {
          for (int tries = 0; tries < 1000; tries++) {
            qrow = static_cast<uint32_t>(qr() % vs.Size());
            if (members.count(vs.IdAt(qrow)) == 0) {
              break;
            }
          }
        }
        const float* q = vs.Vector(qrow);
        const auto truth = flat.SearchRows(q, 10, member_rows);
        std::unordered_set<term_id_t> want;
        for (const auto& n : truth) {
          want.insert(n.id);
        }
        {
          size_t in = 0;
          const auto top = flat.Search(q, 100);
          for (const auto& n : top) {
            in += members.count(n.id);
          }
          inside_share += static_cast<double>(in) / 100.0;
          queries++;
        }
        for (const S s : strategies) {
          // Warm, then time three runs.
          (void)search.Similar(q, 10, c, s, params);
          const auto t = Clock::now();
          HybridSearch::Result res;
          for (int rep = 0; rep < 3; rep++) {
            res = search.Similar(q, 10, c, s, params);
          }
          const double us = Seconds(t) * 1e6 / 3.0;
          size_t hit = 0;
          for (const auto& n : res.neighbors) {
            hit += want.count(n.id);
          }
          auto& a = acc[s];
          a.recall.push_back(want.empty() ? 1.0 : static_cast<double>(hit) / static_cast<double>(want.size()));
          a.us.push_back(us);
          a.scored.push_back(static_cast<double>(res.stats.scored));
          a.members.push_back(static_cast<double>(res.members));
          a.rounds.push_back(static_cast<double>(res.rounds));
        }
      }
      for (const S s : strategies) {
        const auto& a = acc[s];
        std::printf("| %s | %s | %s | %.3f | %.0f | %.0f | %.0f | %.1f |\n", names[bucket], inside ? "inside C" : "outside C", HybridSearch::Name(s), Mean(a.recall), Mean(a.us), Mean(a.scored), Mean(a.members), Mean(a.rounds));
      }
      std::printf("<!-- bucket %s %s: %.3f of the unfiltered top 100 are under C -->\n", names[bucket], inside ? "inside" : "outside", inside_share / queries);
    }
  }
  fs::remove(tmp);
  return 0;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  if (argc < 2) {
    std::cerr << "usage: ontodb_bench_semantic order|labels|types|knn|hybrid ...\n";
    return 2;
  }
  const std::string cmd = argv[1];
  if (cmd == "order") return RunOrder(argc, argv);
  if (cmd == "labels") return RunLabels(argc, argv);
  if (cmd == "types") return RunTypes(argc, argv);
  if (cmd == "knn") return RunKnn(argc, argv);
  if (cmd == "hybrid") return RunHybrid(argc, argv);
  std::cerr << "unknown experiment " << cmd << "\n";
  return 2;
}
