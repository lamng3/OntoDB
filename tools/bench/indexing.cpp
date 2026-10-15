// Which permutations pay for themselves? Loads a dataset into an indexed
// store once per permutation set, then runs the eight bound/unbound pattern
// shapes on a cold pool and reports space, keys examined, and page reads.
//
//   ontodb_bench_indexing <work dir> <file.owl|.ttl>...
//
// Prints a Markdown table to stdout. See benchmarks/indexing/question.md.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "buffer/buffer_pool_manager.h"
#include "catalog/catalog.h"
#include "common/io_stats.h"
#include "dictionary/disk_dictionary.h"
#include "loader/rdf_loader.h"
#include "store/indexed_store.h"
#include "store/indexed_triple_iterator.h"

namespace fs = std::filesystem;
using namespace ontodb;  // NOLINT

namespace {

constexpr size_t kLoadPool = 4096;
constexpr size_t kQueryPool = 64;
constexpr int kSamples = 20;
constexpr const char* kShapes[] = {"---", "s--", "-p-", "--o", "sp-", "s-o", "-po", "spo"};

struct Shape {
  double results = 0;
  double examined = 0;
  double reads = 0;
};

auto PatternFor(const Triple& t, int mask) -> TriplePattern {
  TriplePattern p;
  if ((mask & 4) != 0)
    p.subject = t.subject;
  if ((mask & 2) != 0)
    p.predicate = t.predicate;
  if ((mask & 1) != 0)
    p.object = t.object;
  return p;
}

// The eight shapes as masks over (s, p, o), in kShapes order.
constexpr int kMasks[] = {0, 4, 2, 1, 6, 5, 3, 7};

}  // namespace

auto main(int argc, char** argv) -> int {
  if (argc < 3) {
    std::cerr << "usage: ontodb_bench_indexing <work dir> <rdf file>...\n";
    return 2;
  }
  const fs::path work = argv[1];
  fs::create_directories(work);
  const std::vector<std::vector<std::string>> sets = {
      {"spo"}, {"spo", "pos", "osp"}, {"spo", "sop", "pso", "pos", "osp", "ops"}};

  std::cout << "| Permutations | Load (s) | File (MiB) |";
  for (const char* shape : kShapes)
    std::cout << ' ' << shape << " |";
  std::cout << "\n|---|---|---|";
  for (size_t i = 0; i < std::size(kShapes); i++)
    std::cout << "---|";
  std::cout << '\n';

  std::vector<Triple> samples;
  for (const auto& set : sets) {
    std::string label;
    for (const auto& name : set)
      label += (label.empty() ? "" : " ") + name;
    const fs::path file = work / ("db-" + std::to_string(set.size()));
    fs::remove(file);

    const auto started = std::chrono::steady_clock::now();
    uint64_t triples = 0;
    {
      DiskManager disk(file.string());
      BufferPoolManager pool(kLoadPool, &disk, 2);
      Catalog catalog(&pool);
      DiskDictionary dictionary(&pool, &catalog);
      IndexedStore store(&catalog, &pool, &dictionary, set);
      for (int i = 2; i < argc; i++) {
        triples += RdfLoader().Load(argv[i], dictionary, store).triples;
      }
      if (samples.empty()) {  // the same sampled triples for every set
        std::vector<Triple> all;
        auto scan = store.Scan({});
        Triple t;
        while (scan->Next(&t))
          all.push_back(t);
        std::mt19937 rng(1);
        for (int i = 0; i < kSamples; i++)
          samples.push_back(all[rng() % all.size()]);
      }
      pool.FlushAllPages();
      disk.ShutDown();
    }
    const double load_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const double mib = static_cast<double>(fs::file_size(file)) / (1024.0 * 1024.0);

    std::cout << "| " << label << " | " << load_s << " | " << mib << " |";
    for (const int mask : kMasks) {
      Shape shape;
      const int runs = mask == 0 ? 1 : kSamples;
      for (int r = 0; r < runs; r++) {
        IoStats stats;
        DiskManager disk(file.string(), &stats);
        BufferPoolManager pool(kQueryPool, &disk, 2, nullptr, &stats);
        Catalog catalog(&pool);
        DiskDictionary dictionary(&pool, &catalog);
        IndexedStore store(&catalog, &pool, &dictionary);
        const auto before = stats.page_reads();
        auto scan = store.Scan(PatternFor(samples[r], mask));
        Triple t;
        while (scan->Next(&t))
          shape.results++;
        shape.examined +=
            static_cast<double>(dynamic_cast<IndexedTripleIterator&>(*scan).KeysExamined());
        scan.reset();
        shape.reads += static_cast<double>(stats.page_reads() - before);
      }
      std::printf(" %.0f / %.0f / %.0f |", shape.results / runs, shape.examined / runs,
                  shape.reads / runs);
      std::fflush(stdout);
    }
    std::cout << "  <!-- " << triples << " triples -->\n";
  }
  return 0;
}
