// How OntoDB scales across threads (benchmarks/compare, concurrency).
//
//   ontodb_bench_concurrency <work dir> <queries.tsv> <seconds> <rdf file>...
//
// Loads the files into an indexed store, then for each thread count measures
//   read     T reader threads, each running sampled queries of one kind in a loop
//   insert   T writer threads, each inserting triples that are new to the store
//   mixed    one writer thread beside T reader threads
// for `seconds` each, and prints one line per run: workload, kind, threads, reads/s, inserts/s.
// A query is one execution through the engine with rows decoded to strings. Queries come from
// the same list the other engines run; "lookup" is the shapes (s,.,.) and (s,p,.), "range" is
// (.,p,o). Writers insert through the store (three B+ trees, latch crabbing) without the log.

#include <atomic>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "binder/binder.h"
#include "buffer/buffer_pool_manager.h"
#include "catalog/catalog.h"
#include "dictionary/disk_dictionary.h"
#include "execution/execution_engine.h"
#include "loader/rdf_loader.h"
#include "parser/parser.h"
#include "stats/statistics.h"
#include "store/indexed_store.h"

namespace fs = std::filesystem;
using namespace ontodb;  // NOLINT

namespace {

using Clock = std::chrono::steady_clock;
constexpr size_t kPool = 262144;  // 1 GiB of frames: the data and the inserted triples stay resident

struct Run {
  double reads_per_s{0};
  double inserts_per_s{0};
};

}  // namespace

auto main(int argc, char** argv) -> int {
  if (argc < 5) {
    std::cerr << "usage: ontodb_bench_concurrency <work dir> <queries.tsv> <seconds> <rdf file>...\n";
    return 2;
  }
  const fs::path work = argv[1];
  const double seconds = std::stod(argv[3]);
  fs::create_directories(work);
  const fs::path file = work / "db";
  fs::remove(file);
  DiskManager disk(file.string());
  // CONC_SHARDS sets the buffer pool's shard count (default 16); 1 is one global latch.
  const char* shards_env = std::getenv("CONC_SHARDS");
  const size_t shards = shards_env != nullptr ? std::stoul(shards_env) : 16;
  BufferPoolManager pool(kPool, &disk, 2, nullptr, nullptr, nullptr, shards);
  Catalog catalog(&pool);
  DiskDictionary dictionary(&pool, &catalog);
  IndexedStore store(&catalog, &pool, &dictionary);
  std::vector<fs::path> paths;
  for (int i = 4; i < argc; i++) {
    paths.emplace_back(argv[i]);
  }
  RdfLoader().LoadFiles(paths, dictionary, store);
  Statistics statistics;
  statistics.Collect(store, dictionary);

  std::vector<BoundQuery> lookup;
  std::vector<BoundQuery> range;
  {
    std::ifstream in(argv[2]);
    std::string line;
    while (std::getline(in, line)) {
      const auto tab = line.find('\t');
      if (tab == std::string::npos) {
        continue;
      }
      const std::string name = line.substr(0, tab);
      const std::string kind = name.substr(0, name.find('#'));
      if (kind != "s--" && kind != "sp-" && kind != "-po") {
        continue;
      }
      try {
        auto bound = Binder(dictionary).Bind(Parser(line.substr(tab + 1)).Parse());
        (kind == "-po" ? range : lookup).push_back(std::move(bound));
      } catch (const std::exception&) {
      }
    }
  }

  const term_id_t fresh_base = static_cast<term_id_t>(dictionary.Size()) + 1000000;
  std::atomic<uint64_t> next_fresh{0};

  // Runs `readers` query threads over `queries` and `writers` insert threads for `seconds`.
  const auto run = [&](const std::vector<BoundQuery>& queries, int readers, int writers) {
    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> reads{0};
    std::atomic<uint64_t> inserts{0};
    std::vector<std::thread> threads;
    for (int r = 0; r < readers; r++) {
      threads.emplace_back([&, r] {
        uint64_t mine = 0;
        size_t at = static_cast<size_t>(r) * 7;
        while (!go.load()) {
        }
        while (!stop.load()) {
          Config config;
          IoStats io;
          NullTraceSink trace;
          ExecutorContext ctx(&store, &dictionary, &config, nullptr, nullptr, nullptr, &pool, &io, &trace,
                              nullptr);
          ExecutionEngine engine(&ctx, &statistics);
          engine.Execute(queries[at % queries.size()]);
          at++;
          mine++;
        }
        reads += mine;
      });
    }
    for (int w = 0; w < writers; w++) {
      threads.emplace_back([&] {
        uint64_t mine = 0;
        while (!go.load()) {
        }
        while (!stop.load()) {
          // Multiplying by an odd constant modulo 2^26 is a bijection, so the new subjects are
          // distinct and arrive in scattered order, as keys of a real workload do.
          const term_id_t id = fresh_base + ((next_fresh.fetch_add(1) * 2654435761ULL) & ((1ULL << 26) - 1));
          store.Insert(Triple{id, 1, id % 977 + 1});
          mine++;
        }
        inserts += mine;
      });
    }
    const auto started = Clock::now();
    go = true;
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    stop = true;
    for (auto& t : threads) {
      t.join();
    }
    const double elapsed = std::chrono::duration<double>(Clock::now() - started).count();
    return Run{static_cast<double>(reads) / elapsed, static_cast<double>(inserts) / elapsed};
  };

  const char* only_env = std::getenv("CONC_ONLY");  // e.g. "read-lookup": run one workload only
  const std::string only = only_env != nullptr ? only_env : "";
  const auto wanted = [&](const std::string& name) { return only.empty() || only == name; };
  std::printf("#load\ttriples=%llu\tlookup_queries=%zu\trange_queries=%zu\n",
              static_cast<unsigned long long>(statistics.TripleCount()), lookup.size(), range.size());
  for (const int t : {1, 2, 4, 8}) {
    if (!wanted("read-lookup")) break;
    const Run a = run(lookup, t, 0);
    std::printf("read\tlookup\t%d\t%.0f\t0\n", t, a.reads_per_s);
    std::fflush(stdout);
  }
  for (const int t : {1, 2, 4, 8}) {
    if (!wanted("read-range")) break;
    const Run a = run(range, t, 0);
    std::printf("read\trange\t%d\t%.0f\t0\n", t, a.reads_per_s);
    std::fflush(stdout);
  }
  for (const int t : {1, 2, 4, 8}) {
    if (!wanted("insert")) break;
    const Run a = run(lookup, 0, t);
    std::printf("insert\t-\t%d\t0\t%.0f\n", t, a.inserts_per_s);
    std::fflush(stdout);
  }
  for (const int t : {1, 2, 4, 8}) {
    if (!wanted("mixed")) break;
    const Run a = run(lookup, t, 1);
    std::printf("mixed\tlookup\t%d\t%.0f\t%.0f\n", t, a.reads_per_s, a.inserts_per_s);
    std::fflush(stdout);
  }
  return 0;
}
