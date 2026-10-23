// OntoDB's side of the comparison with other triple stores (benchmarks/compare).
//
//   ontodb_bench_compare <work dir> <queries.tsv> <runs> <rdf file>...
//
// Loads the files into an indexed store (SPO, POS, OSP) with the bulk loader, then runs every
// query of <queries.tsv> (name TAB one-line SPARQL) once to warm and `runs` times to time, and
// prints one line per query: name, rows, median microseconds. Rows are decoded to strings, the
// way the shell would print them. The first line reports the load.
//
// The other side is benchmarks/compare/oxigraph, which reads the same files and queries.

#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "binder/binder.h"
#include "buffer/buffer_pool_manager.h"
#include "catalog/catalog.h"
#include "common/exception.h"
#include "dictionary/disk_dictionary.h"
#include "execution/execution_engine.h"
#include "loader/rdf_loader.h"
#include "semantic/class_hierarchy.h"
#include "semantic/type_closure.h"
#include "semantic/type_entailment_index.h"
#include "parser/parser.h"
#include "stats/statistics.h"
#include "store/indexed_store.h"

namespace fs = std::filesystem;
using namespace ontodb;  // NOLINT

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kPool = 65536;  // 256 MiB of frames: holds LUBM(10), so this measures the engine, not the disk
constexpr int kTimeoutSeconds = 60;

auto Seconds(Clock::time_point since) -> double {
  return std::chrono::duration<double>(Clock::now() - since).count();
}

auto PeakRssMib() -> double {
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  return static_cast<double>(usage.ru_maxrss) / (1024.0 * 1024.0);  // bytes on macOS
}

}  // namespace

auto main(int argc, char** argv) -> int {
  if (argc < 5) {
    std::cerr << "usage: ontodb_bench_compare <work dir> <queries.tsv> <runs> <rdf file>...\n";
    return 2;
  }
  const fs::path work = argv[1];
  const int runs = std::stoi(argv[3]);
  fs::create_directories(work);
  const fs::path file = work / "db";
  fs::remove(file);

  DiskManager disk(file.string());
  BufferPoolManager pool(kPool, &disk, 2);
  Catalog catalog(&pool);
  // ONTODB_TERM_CACHE sets the dictionary's decode cache in entries; 0 turns it off.
  const char* cache_env = std::getenv("ONTODB_TERM_CACHE");
  DiskDictionary dictionary(&pool, &catalog,
                            cache_env != nullptr ? std::stoul(cache_env) : DiskDictionary::kDefaultCacheSlots);
  IndexedStore store(&catalog, &pool, &dictionary);
  std::vector<fs::path> paths;
  for (int i = 4; i < argc; i++) {
    paths.emplace_back(argv[i]);
  }
  const auto load_started = Clock::now();
  const auto loaded = RdfLoader().LoadFiles(paths, dictionary, store);
  pool.FlushAllPages();
  const double load_s = Seconds(load_started);
  // COMPARE_INFERENCE: "plain" (the default) answers `(x rdf:type C)` from the stored triples;
  // "entail" adds RDFS subclass entailment by interval scans (\set entail on); "materialize"
  // stores the entailed type triples first. The ontology must be among the loaded files.
  const char* inference_env = std::getenv("COMPARE_INFERENCE");
  const std::string inference = inference_env != nullptr ? inference_env : "plain";
  TypeEntailmentIndex entailment;
  double inference_s = 0;
  uint64_t inferred = 0;
  if (inference == "entail") {
    const auto started = Clock::now();
    entailment.Refresh(&store, &dictionary, 1);
    inference_s = Seconds(started);
  } else if (inference == "materialize") {
    const auto started = Clock::now();
    const auto type = dictionary.Lookup("<http://www.w3.org/1999/02/22-rdf-syntax-ns#type>");
    const auto sub = dictionary.Lookup("<http://www.w3.org/2000/01/rdf-schema#subClassOf>");
    if (type.has_value() && sub.has_value()) {
      const auto h = ClassHierarchy::Build(CollectSubClassEdges(&store, *sub), ClassHierarchy::TreeParent::kDeepest,
                                           CollectTypeClasses(&store, *type));
      inferred = MaterializeTypes(h, &store, *type);
    }
    pool.FlushAllPages();
    inference_s = Seconds(started);
  }
  const auto stats_started = Clock::now();
  Statistics statistics;
  statistics.Collect(store, dictionary);
  const double stats_s = Seconds(stats_started);
  std::printf("#load\ttriples=%llu\tload_s=%.2f\tstatistics_s=%.2f\tinference=%s\tinference_s=%.2f\tinferred=%llu\tinference_mib=%.1f\tfile_mib=%.1f\tpeak_rss_mib=%.0f\n",
              static_cast<unsigned long long>(loaded.triples), load_s, stats_s, inference.c_str(), inference_s,
              static_cast<unsigned long long>(inferred), static_cast<double>(entailment.IndexBytes()) / (1024.0 * 1024.0),
              static_cast<double>(fs::file_size(file)) / (1024.0 * 1024.0), PeakRssMib());

  std::ifstream in(argv[2]);
  std::string line;
  while (std::getline(in, line)) {
    const auto tab = line.find('\t');
    if (tab == std::string::npos) {
      continue;
    }
    const std::string name = line.substr(0, tab);
    const std::string text = line.substr(tab + 1);
    try {
      const BoundQuery bound = Binder(dictionary).Bind(Parser(text).Parse());
      size_t rows = 0;
      const auto once = [&]() {
        Config config;
        config.entail = inference == "entail";
        IoStats io;
        NullTraceSink trace;
        ExecutorContext ctx(&store, &dictionary, &config, nullptr, nullptr, nullptr, &pool, &io,
                            &trace, nullptr);
        if (inference == "entail") {
          ctx.SetEntailment(&entailment);
        }
        ctx.SetDeadline(Clock::now() + std::chrono::seconds(kTimeoutSeconds));
        ExecutionEngine engine(&ctx, &statistics);
        const auto started = Clock::now();
        rows = engine.Execute(bound).rows.size();
        return Seconds(started) * 1e6;
      };
      const double first = once();  // warms the pool
      std::vector<double> times;
      const int n = first > 2e6 ? 1 : runs;
      for (int r = 0; r < n; r++) {
        times.push_back(once());
      }
      std::sort(times.begin(), times.end());
      std::printf("%s\t%zu\t%.1f\n", name.c_str(), rows, times[times.size() / 2]);
    } catch (const QueryTimeoutException&) {
      std::printf("%s\t-1\ttimeout\n", name.c_str());
    } catch (const std::exception& e) {
      std::printf("%s\t-1\terror: %s\n", name.c_str(), e.what());
    }
    std::fflush(stdout);
  }
  return 0;
}
