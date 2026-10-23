// Vector search inside an ontology store, on any ontology (benchmarks/vector).
//
//   ontodb_bench_vector filtered <name> <ontology.nt> <terms.ovec> <synonyms.ovec|-> <out dir>
//   ontodb_bench_vector index    <name> <ontology.nt> <terms.ovec> <out dir>
//
// filtered: for classes of five subtree-size buckets, the ten nearest terms under a class for a
// query inside the class, one outside it, and (with a synonyms file) the embedding of a synonym of
// a term inside it, which is not a stored vector. Every strategy of HybridSearch runs; recall is
// against exact search over the class's members. Writes <name>-hybrid.tsv (one line per class,
// query kind, and strategy), <name>-workload.tsv (the queries, for baselines in other systems), and
// <name>-build.tsv. A query term is never counted as its own neighbor.
//
// index: recall against time for IVF (by nprobe) and HNSW (by ef), against exact search, over 1,000
// query terms; writes <name>-index.tsv.

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "buffer/buffer_pool_manager.h"
#include "dictionary/mem_dictionary.h"
#include "loader/rdf_loader.h"
#include "semantic/class_hierarchy.h"
#include "semantic/flat_index.h"
#include "semantic/hnsw_index.h"
#include "semantic/hybrid_search.h"
#include "semantic/ivf_index.h"
#include "semantic/label_index.h"
#include "semantic/type_closure.h"
#include "semantic/vector_store.h"
#include "storage/disk/disk_manager.h"
#include "store/mem_store.h"

namespace fs = std::filesystem;
using namespace ontodb;  // NOLINT
using Clock = std::chrono::steady_clock;

namespace {

constexpr const char* kSubClassOf = "<http://www.w3.org/2000/01/rdf-schema#subClassOf>";

auto Seconds(Clock::time_point since) -> double {
  return std::chrono::duration<double>(Clock::now() - since).count();
}

struct World {
  MemDictionary dict;
  MemStore store;
  term_id_t sub{0};
  void Load(const std::string& nt) {
    RdfLoader().LoadFiles({nt}, dict, store);
    sub = dict.Insert(kSubClassOf);
  }
};

// The records of an OVEC1 file: spelling and vector, without the dictionary (a synonym's spelling
// is the term it belongs to, and several synonyms share a term).
struct RawVectors {
  uint32_t dim{0};
  std::vector<std::string> spellings;
  std::vector<float> data;
};

auto ReadRaw(const std::string& path) -> RawVectors {
  std::ifstream in(path, std::ios::binary);
  char magic[6];
  in.read(magic, 6);
  RawVectors raw;
  uint64_t count = 0;
  in.read(reinterpret_cast<char*>(&raw.dim), 4);
  in.read(reinterpret_cast<char*>(&count), 8);
  for (uint64_t i = 0; i < count; i++) {
    uint32_t len = 0;
    in.read(reinterpret_cast<char*>(&len), 4);
    std::string spelling(len, '\0');
    in.read(spelling.data(), len);
    raw.spellings.push_back(std::move(spelling));
    const size_t at = raw.data.size();
    raw.data.resize(at + raw.dim);
    in.read(reinterpret_cast<char*>(raw.data.data() + at), static_cast<std::streamsize>(raw.dim) * 4);
  }
  return raw;
}

auto Bucket(size_t members) -> int {
  return members <= 10 ? 0 : members <= 100 ? 1 : members <= 1000 ? 2 : members <= 5000 ? 3 : 4;
}
constexpr const char* kBucketName[] = {"<=10", "11-100", "101-1000", "1001-5000", ">5000"};

// Rows of the vector store that belong to the classes below `c`.
auto MemberRows(const ClassHierarchy& h, const VectorStore& vs, term_id_t c) -> std::vector<uint32_t> {
  std::vector<uint32_t> rows;
  for (const term_id_t id : h.Descendants(c)) {
    if (const auto r = vs.Row(id)) {
      rows.push_back(*r);
    }
  }
  return rows;
}

auto RunFiltered(int argc, char** argv) -> int {
  if (argc < 7) {
    std::cerr << "usage: filtered <name> <ontology.nt> <terms.ovec> <synonyms.ovec|-> <out dir>\n";
    return 2;
  }
  const std::string name = argv[2];
  const fs::path out = argv[6];
  fs::create_directories(out);
  World w;
  w.Load(argv[3]);
  VectorStore vs(VectorStore::PeekDimension(argv[4]));
  vs.Load(argv[4], &w.dict);
  const auto edges = CollectSubClassEdges(&w.store, w.sub);
  const auto h = ClassHierarchy::Build(edges);

  // Synonym query vectors by the term they name.
  RawVectors syn;
  std::unordered_map<term_id_t, std::vector<size_t>> syn_of;
  if (std::string(argv[5]) != "-") {
    syn = ReadRaw(argv[5]);
    for (size_t i = 0; i < syn.spellings.size(); i++) {
      if (const auto id = w.dict.Lookup(syn.spellings[i])) {
        syn_of[*id].push_back(i);
      }
    }
  }

  const fs::path tmp = fs::temp_directory_path() / ("ontodb-vector-" + std::to_string(::getpid()) + ".db");
  fs::remove(tmp);
  DiskManager disk(tmp.string());
  BufferPoolManager pool(65536, &disk, 2, nullptr, nullptr, nullptr, 16);
  std::ofstream build(out / (name + "-build.tsv"));
  build << "what\tseconds\tmib\n";
  LabelIndex classes(&pool);
  {
    const auto t = Clock::now();
    std::vector<std::pair<uint32_t, term_id_t>> pr;
    for (uint32_t row = 0; row < vs.Size(); row++) {
      if (h.Label(vs.IdAt(row)) != 0) {
        pr.emplace_back(h.Label(vs.IdAt(row)), vs.IdAt(row));
      }
    }
    classes.BulkLoad(pr);
    build << "intervals+label tree\t" << Seconds(t) << "\t0\n";
  }
  FlatIndex flat(&vs, FlatIndex::Precision::kFloat32);
  const uint32_t lists = std::max<uint32_t>(64, static_cast<uint32_t>(2.6 * std::sqrt(static_cast<double>(vs.Size()))));
  IvfIndex ivf(&vs, &pool, {.lists = lists});
  {
    const auto t = Clock::now();
    ivf.Build();
    build << "ivf (" << lists << " lists)\t" << Seconds(t) << "\t" << static_cast<double>(ivf.Bytes()) / 1048576.0 << "\n";
  }
  HnswIndex hnsw(&vs, {.m = 16, .ef_construction = 200});
  {
    const auto t = Clock::now();
    hnsw.Build();
    build << "hnsw (M=16)\t" << Seconds(t) << "\t" << static_cast<double>(hnsw.Bytes()) / 1048576.0 << "\n";
  }
  build << "flat\t0\t" << static_cast<double>(flat.Bytes()) / 1048576.0 << "\n";
  build.close();
  HybridSearch search({&h, &classes, &vs, &flat, &ivf, &hnsw});
  const HybridSearch::Params params{.nprobe = 16, .ef = 64, .pre_threshold = 4096, .max_visits = 0};

  // Classes with subclasses and a vector, 20 per subtree-size bucket.
  std::vector<term_id_t> parents;
  for (const auto& e : edges) {
    parents.push_back(e.second);
  }
  std::sort(parents.begin(), parents.end());
  parents.erase(std::unique(parents.begin(), parents.end()), parents.end());
  std::mt19937_64 rng(7);
  std::shuffle(parents.begin(), parents.end(), rng);
  std::map<int, std::vector<term_id_t>> sample;
  for (const term_id_t c : parents) {
    if (!vs.Row(c).has_value()) {
      continue;
    }
    auto& v = sample[Bucket(h.DescendantCount(c))];
    if (v.size() < 20) {
      v.push_back(c);
    }
  }

  std::ofstream result(out / (name + "-hybrid.tsv"));
  std::ofstream workload(out / (name + "-workload.tsv"));
  result << "ontology\tbucket\tkind\tclass\tstrategy\trecall\tus\tscored\tmembers\tunder_share\n";
  workload << "bucket\tkind\tclass\tquery\n";
  using S = HybridSearch::Strategy;
  const S strategies[] = {S::kPreFilter, S::kPostFilterHnsw, S::kPostFilterIvf, S::kMaskHnsw, S::kMaskIvf, S::kAuto};
  // VEC_K sets the number of neighbors asked for (default 10).
  const size_t kK = std::getenv("VEC_K") != nullptr ? std::stoul(std::getenv("VEC_K")) : 10;
  struct Job {
    term_id_t cls;
    std::vector<float> query;
  };
  std::vector<Job> jobs;  // every query of the workload, for the thread-scaling run below
  for (const auto& [bucket, in_bucket] : sample) {
    for (const term_id_t c : in_bucket) {
      const auto member_rows = MemberRows(h, vs, c);
      if (member_rows.empty()) {
        continue;
      }
      std::unordered_set<uint32_t> member_set(member_rows.begin(), member_rows.end());
      std::mt19937_64 qr(c);
      for (const std::string kind : {"inside", "outside", "synonym"}) {
        std::vector<float> synthetic;
        const float* q = nullptr;
        int self_row = -1;
        std::string ref;
        if (kind == "inside") {
          self_row = static_cast<int>(member_rows[qr() % member_rows.size()]);
          q = vs.Vector(static_cast<uint32_t>(self_row));
          ref = "row:" + std::to_string(self_row);
        } else if (kind == "outside") {
          uint32_t row = 0;
          for (int tries = 0; tries < 1000; tries++) {
            row = static_cast<uint32_t>(qr() % vs.Size());
            if (member_set.count(row) == 0) {
              break;
            }
          }
          q = vs.Vector(row);
          ref = "row:" + std::to_string(row);
        } else {
          if (syn_of.empty()) {
            continue;
          }
          // A synonym of a random member term that has one.
          int found = -1;
          for (int tries = 0; tries < 200 && found < 0; tries++) {
            const uint32_t row = member_rows[qr() % member_rows.size()];
            const auto it = syn_of.find(vs.IdAt(row));
            if (it != syn_of.end()) {
              found = static_cast<int>(it->second[qr() % it->second.size()]);
            }
          }
          if (found < 0) {
            continue;
          }
          synthetic = Normalized(syn.data.data() + static_cast<size_t>(found) * syn.dim, syn.dim);
          q = synthetic.data();
          ref = "syn:" + std::to_string(found);
        }
        jobs.push_back({c, std::vector<float>(q, q + vs.Dim())});
        workload << kBucketName[bucket] << '\t' << kind << '\t' << w.dict.Lookup(c).value_or("?") << '\t' << ref << '\n';
        // Ten nearest members, the query term itself excluded.
        auto drop_self = [&](std::vector<Neighbor> v) {
          std::vector<Neighbor> keep;
          for (const auto& n : v) {
            if (self_row >= 0 && n.id == vs.IdAt(static_cast<uint32_t>(self_row))) {
              continue;
            }
            keep.push_back(n);
          }
          if (keep.size() > kK) {
            keep.resize(kK);
          }
          return keep;
        };
        const auto truth = drop_self(flat.SearchRows(q, kK + 1, member_rows));
        std::unordered_set<term_id_t> want;
        for (const auto& n : truth) {
          want.insert(n.id);
        }
        // How much of the unfiltered top 100 lies under the class.
        size_t under = 0;
        for (const auto& n : flat.Search(q, 100)) {
          under += member_set.count(vs.Row(n.id).value_or(0)) != 0 ? 1 : 0;
        }
        // Resolve-then-mask: the members by range scans, a bitmap, then a masked HNSW walk. This is
        // what a store without an interval predicate does, and the timing includes resolving.
        {
          auto run_bitmap = [&]() {
            RowMask mask(vs.Size(), 0);
            for (const term_id_t id : classes.Scan(h.Intervals(c))) {
              if (const auto r = vs.Row(id)) {
                mask[*r] = 1;
              }
            }
            const RowFilter filter(&mask);
            return hnsw.Search(q, kK + 1, params.ef, &filter);
          };
          (void)run_bitmap();
          std::vector<Neighbor> res;
          const auto t = Clock::now();
          for (int rep = 0; rep < 3; rep++) {
            res = run_bitmap();
          }
          const double us = Seconds(t) * 1e6 / 3.0;
          size_t hit = 0;
          const auto got = drop_self(res);
          for (const auto& n : got) {
            hit += want.count(n.id);
          }
          result << name << '\t' << kBucketName[bucket] << '\t' << kind << '\t' << c << "\tmask-hnsw/bitmap\t"
                 << (want.empty() ? 1.0 : static_cast<double>(hit) / static_cast<double>(want.size())) << '\t' << us << "\t0\t0\t"
                 << static_cast<double>(under) / 100.0 << '\n';
        }
        // The auto rule at other thresholds, to see where the crossover really is.
        for (const size_t threshold : {256u, 1024u, 4096u, 16384u, 65536u}) {
          HybridSearch::Params p = params;
          p.pre_threshold = threshold;
          (void)search.Similar(q, kK + 1, c, S::kAuto, p);
          HybridSearch::Result res;
          const auto t = Clock::now();
          for (int rep = 0; rep < 3; rep++) {
            res = search.Similar(q, kK + 1, c, S::kAuto, p);
          }
          const double us = Seconds(t) * 1e6 / 3.0;
          size_t hit = 0;
          for (const auto& n : drop_self(res.neighbors)) {
            hit += want.count(n.id);
          }
          result << name << '\t' << kBucketName[bucket] << '\t' << kind << '\t' << c << "\tauto@" << threshold << '\t'
                 << (want.empty() ? 1.0 : static_cast<double>(hit) / static_cast<double>(want.size())) << '\t' << us << "\t0\t0\t"
                 << static_cast<double>(under) / 100.0 << '\n';
        }
        for (const S s : strategies) {
          (void)search.Similar(q, kK + 1, c, s, params);  // warm
          HybridSearch::Result res;
          const auto t = Clock::now();
          for (int rep = 0; rep < 3; rep++) {
            res = search.Similar(q, kK + 1, c, s, params);
          }
          const double us = Seconds(t) * 1e6 / 3.0;
          const auto got = drop_self(res.neighbors);
          size_t hit = 0;
          for (const auto& n : got) {
            hit += want.count(n.id);
          }
          const double recall = want.empty() ? 1.0 : static_cast<double>(hit) / static_cast<double>(want.size());
          result << name << '\t' << kBucketName[bucket] << '\t' << kind << '\t' << c << '\t' << HybridSearch::Name(s) << '\t' << recall << '\t' << us
                 << '\t' << res.stats.scored << '\t' << res.members << '\t' << static_cast<double>(under) / 100.0 << '\n';
        }
      }
    }
  }
  // Threads: the auto rule over the whole workload, queries shared by every thread.
  {
    std::ofstream scale(out / (name + "-scale.tsv"));
    scale << "ontology\tthreads\tqueries_per_s\n";
    for (const int threads : {1, 2, 4, 8}) {
      std::atomic<bool> stop{false};
      std::atomic<uint64_t> done{0};
      std::vector<std::thread> pool_threads;
      for (int t = 0; t < threads; t++) {
        pool_threads.emplace_back([&, t] {
          uint64_t mine = 0;
          size_t at = static_cast<size_t>(t) * 17;
          while (!stop.load()) {
            const Job& job = jobs[at % jobs.size()];
            (void)search.Similar(job.query.data(), kK + 1, job.cls, S::kAuto, params);
            at++;
            mine++;
          }
          done += mine;
        });
      }
      const auto t0 = Clock::now();
      std::this_thread::sleep_for(std::chrono::seconds(3));
      stop = true;
      for (auto& th : pool_threads) {
        th.join();
      }
      scale << name << '\t' << threads << '\t' << static_cast<double>(done) / Seconds(t0) << '\n';
    }
  }
  fs::remove(tmp);
  return 0;
}

auto RunIndex(int argc, char** argv) -> int {
  if (argc < 6) {
    std::cerr << "usage: index <name> <ontology.nt> <terms.ovec> <out dir>\n";
    return 2;
  }
  const std::string name = argv[2];
  const fs::path out = argv[5];
  fs::create_directories(out);
  World w;
  w.Load(argv[3]);
  VectorStore vs(VectorStore::PeekDimension(argv[4]));
  vs.Load(argv[4], &w.dict);
  const fs::path tmp = fs::temp_directory_path() / ("ontodb-vector-idx-" + std::to_string(::getpid()) + ".db");
  fs::remove(tmp);
  DiskManager disk(tmp.string());
  BufferPoolManager pool(65536, &disk, 2, nullptr, nullptr, nullptr, 16);
  FlatIndex flat(&vs, FlatIndex::Precision::kFloat32);
  const uint32_t lists = std::max<uint32_t>(64, static_cast<uint32_t>(2.6 * std::sqrt(static_cast<double>(vs.Size()))));
  IvfIndex ivf(&vs, &pool, {.lists = lists});
  double ivf_s = 0;
  double hnsw_s = 0;
  {
    const auto t = Clock::now();
    ivf.Build();
    ivf_s = Seconds(t);
  }
  HnswIndex hnsw(&vs, {.m = 16, .ef_construction = 200});
  {
    const auto t = Clock::now();
    hnsw.Build();
    hnsw_s = Seconds(t);
  }
  std::mt19937_64 rng(3);
  std::vector<uint32_t> queries;
  for (int i = 0; i < 1000; i++) {
    queries.push_back(static_cast<uint32_t>(rng() % vs.Size()));
  }
  std::vector<std::unordered_set<uint32_t>> truth;
  double flat_us = 0;
  {
    const auto t = Clock::now();
    for (const uint32_t row : queries) {
      std::unordered_set<uint32_t> want;
      for (const auto& n : flat.Search(vs.Vector(row), 11)) {
        if (n.id != vs.IdAt(row) && want.size() < 10) {
          want.insert(static_cast<uint32_t>(*vs.Row(n.id)));
        }
      }
      truth.push_back(std::move(want));
    }
    flat_us = Seconds(t) * 1e6 / static_cast<double>(queries.size());
  }
  std::ofstream result(out / (name + "-index.tsv"));
  result << "ontology\tindex\tsetting\trecall\tus\tmib\tbuild_s\n";
  result << name << "\tflat\texact\t1\t" << flat_us << '\t' << static_cast<double>(flat.Bytes()) / 1048576.0 << "\t0\n";
  auto measure = [&](const std::string& index, const std::string& setting, double mib, double build_s, auto&& search) {
    for (const uint32_t row : queries) {
      (void)search(vs.Vector(row), 11);  // warm
      break;
    }
    const auto t = Clock::now();
    double recall = 0;
    for (size_t i = 0; i < queries.size(); i++) {
      const uint32_t row = queries[i];
      size_t hit = 0;
      size_t taken = 0;
      for (const auto& n : search(vs.Vector(row), 11)) {
        if (n.id == vs.IdAt(row) || taken >= 10) {
          continue;
        }
        taken++;
        hit += truth[i].count(static_cast<uint32_t>(*vs.Row(n.id)));
      }
      recall += static_cast<double>(hit) / 10.0;
    }
    const double us = Seconds(t) * 1e6 / static_cast<double>(queries.size());
    result << name << '\t' << index << '\t' << setting << '\t' << recall / static_cast<double>(queries.size()) << '\t' << us << '\t' << mib << '\t' << build_s << '\n';
  };
  const double ivf_mib = static_cast<double>(ivf.Bytes()) / 1048576.0;
  for (const uint32_t nprobe : {1u, 2u, 4u, 8u, 16u, 32u, 64u}) {
    measure("ivf", "nprobe=" + std::to_string(nprobe), ivf_mib, ivf_s, [&](const float* q, size_t k) { return ivf.Search(q, k, nprobe); });
  }
  const double hnsw_mib = static_cast<double>(hnsw.Bytes()) / 1048576.0;
  for (const uint32_t ef : {10u, 20u, 40u, 80u, 160u}) {
    measure("hnsw", "ef=" + std::to_string(ef), hnsw_mib, hnsw_s, [&](const float* q, size_t k) { return hnsw.Search(q, k, ef); });
  }
  fs::remove(tmp);
  return 0;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  if (argc < 2) {
    std::cerr << "usage: ontodb_bench_vector filtered|index ...\n";
    return 2;
  }
  const std::string cmd = argv[1];
  if (cmd == "filtered") return RunFiltered(argc, argv);
  if (cmd == "index") return RunIndex(argc, argv);
  std::cerr << "unknown experiment " << cmd << "\n";
  return 2;
}
