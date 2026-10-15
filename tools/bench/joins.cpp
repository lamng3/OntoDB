// Join order and join operator on LUBM. Loads the data into an indexed store
// once, then runs each query under five settings and prints a Markdown table
// of milliseconds (and rows) per query and setting:
//
//   written  the query's own pattern order, nested loops, no optimizer
//   nlj      the optimizer's order, nested loops
//   inlj     the optimizer's order, index nested-loop joins
//   hash     the optimizer's order, hash joins
//   auto     the optimizer's order and its own choice of operator
//
//   ontodb_bench_joins <work dir> <timeout seconds> <query dir> <rdf file>...
//
// A run that passes the timeout is reported as "> N s". See
// benchmarks/joins/question.md.

#include <algorithm>
#include <chrono>
#include <cstdio>
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
#include "parser/parser.h"
#include "stats/statistics.h"
#include "store/indexed_store.h"

namespace fs = std::filesystem;
using namespace ontodb;  // NOLINT

namespace {

constexpr size_t kPool = 8192;  // holds all of LUBM(1): this measures plans, not I/O

// LUBM's published queries put commas between SELECT variables and write some
// IRIs without angle brackets. Make them standard SPARQL.
auto Normalize(std::string text) -> std::string {
  const auto where = text.find("WHERE");
  for (size_t i = 0; i < where && i < text.size(); i++) {
    if (text[i] == ',') {
      text[i] = ' ';
    }
  }
  std::string out;
  for (size_t i = 0; i < text.size();) {
    const bool bare = text.compare(i, 7, "http://") == 0 &&
                      (i == 0 || (text[i - 1] != '<' && text[i - 1] != '"'));
    if (!bare) {
      out.push_back(text[i++]);
      continue;
    }
    size_t end = i;
    while (end < text.size() &&
           std::string_view(" \t\n}<>\"").find(text[end]) == std::string_view::npos) {
      end++;
    }
    out += "<" + text.substr(i, end - i) + ">";
    i = end;
  }
  return out;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  if (argc < 5) {
    std::cerr << "usage: ontodb_bench_joins <work dir> <timeout s> <query dir> <rdf file>...\n";
    return 2;
  }
  const fs::path work = argv[1];
  const int timeout_s = std::stoi(argv[2]);
  const fs::path query_dir = argv[3];
  fs::create_directories(work);
  const fs::path file = work / "db";
  fs::remove(file);

  DiskManager disk(file.string());
  BufferPoolManager pool(kPool, &disk, 2);
  Catalog catalog(&pool);
  DiskDictionary dictionary(&pool, &catalog);
  IndexedStore store(&catalog, &pool, &dictionary);
  uint64_t triples = 0;
  for (int i = 4; i < argc; i++) {
    triples += RdfLoader().Load(argv[i], dictionary, store).triples;
  }
  Statistics statistics;
  statistics.Collect(store, dictionary);

  std::vector<fs::path> queries;
  for (const auto& entry : fs::directory_iterator(query_dir)) {
    if (entry.path().extension() == ".sparql") {
      queries.push_back(entry.path());
    }
  }
  std::sort(queries.begin(), queries.end());

  struct Setting {
    const char* name;
    bool optimize;
    JoinMethod join;
  };
  const Setting settings[] = {{"written", false, JoinMethod::kNestedLoop},
                              {"nlj", true, JoinMethod::kNestedLoop},
                              {"inlj", true, JoinMethod::kIndexNestedLoop},
                              {"hash", true, JoinMethod::kHash},
                              {"auto", true, JoinMethod::kAuto}};
  std::cout << "<!-- " << triples << " triples, timeout " << timeout_s << " s -->\n";
  std::cout << "| Query | Rows |";
  for (const auto& s : settings) {
    std::cout << ' ' << s.name << " (ms) |";
  }
  std::cout << "\n|---|---|";
  for (size_t i = 0; i < std::size(settings); i++) {
    std::cout << "---|";
  }
  std::cout << '\n';

  for (const auto& path : queries) {
    std::ifstream in(path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    const BoundQuery bound = Binder(dictionary).Bind(Parser(Normalize(buffer.str())).Parse());
    std::string rows = "?";
    std::ostringstream cells;
    for (const auto& setting : settings) {
      Config config;
      config.join = setting.join;
      IoStats io;
      NullTraceSink trace;
      ExecutorContext ctx(&store, &dictionary, &config, nullptr, nullptr, nullptr, &pool, &io,
                          &trace, nullptr);
      const auto started = std::chrono::steady_clock::now();
      ctx.SetDeadline(started + std::chrono::seconds(timeout_s));
      try {
        ExecutionEngine engine(&ctx, setting.optimize ? &statistics : nullptr);
        const auto result = engine.Execute(bound);
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        rows = std::to_string(result.rows.size());
        char cell[32];
        std::snprintf(cell, sizeof(cell), " %.1f |", ms);
        cells << cell;
      } catch (const QueryTimeoutException&) {
        cells << " > " << timeout_s << " s |";
      }
    }
    std::cout << "| " << path.stem().string() << " | " << rows << " |" << cells.str() << std::endl;
  }
  return 0;
}
