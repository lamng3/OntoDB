#include "semantic/semantic_engine.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <sstream>
#include <unistd.h>

#include "buffer/buffer_pool_manager.h"
#include "common/term_codec.h"
#include "semantic/class_hierarchy.h"
#include "semantic/flat_index.h"
#include "semantic/hnsw_index.h"
#include "semantic/hybrid_search.h"
#include "semantic/ivf_index.h"
#include "semantic/label_index.h"
#include "semantic/type_closure.h"
#include "semantic/vector_store.h"
#include "storage/disk/disk_manager.h"

namespace ontodb {

namespace {

constexpr const char* kSubClassOf = "<http://www.w3.org/2000/01/rdf-schema#subClassOf>";
constexpr const char* kLabel = "<http://www.w3.org/2000/01/rdf-schema#label>";
constexpr size_t kPoolFrames = 4096;
constexpr size_t kDefaultK = 10;

using Clock = std::chrono::steady_clock;

auto Ms(Clock::time_point since) -> double {
  return std::chrono::duration<double, std::milli>(Clock::now() - since).count();
}

auto Trim(const std::string& s) -> std::string {
  const auto a = s.find_first_not_of(" \t");
  if (a == std::string::npos) {
    return "";
  }
  return s.substr(a, s.find_last_not_of(" \t") - a + 1);
}

// Units of an argument line: `<...>` and `"..."` are one token each, anything
// else is split on blanks.
auto Tokens(const std::string& line) -> std::vector<std::string> {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < line.size()) {
    if (line[i] == ' ' || line[i] == '\t') {
      i++;
      continue;
    }
    const char close = line[i] == '<' ? '>' : line[i] == '"' ? '"' : ' ';
    size_t j = i + 1;
    while (j < line.size() && line[j] != close && !(close == ' ' && line[j] == '\t')) {
      j++;
    }
    if (close != ' ' && j < line.size()) {
      j++;
    }
    out.push_back(line.substr(i, j - i));
    i = j;
  }
  return out;
}

auto IsNumber(const std::string& s) -> bool {
  return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos;
}

}  // namespace

struct SemanticEngine::Impl {
  std::filesystem::path file;
  std::unique_ptr<DiskManager> disk;
  std::unique_ptr<BufferPoolManager> pool;
  std::unique_ptr<ClassHierarchy> hierarchy;
  std::unique_ptr<LabelIndex> classes;
  std::unique_ptr<VectorStore> vectors;
  std::unique_ptr<FlatIndex> flat;
  std::unique_ptr<IvfIndex> ivf;
  std::unique_ptr<HnswIndex> hnsw;
  std::unique_ptr<HybridSearch> search;
  std::string hierarchy_predicate;

  Impl() {
    static std::atomic<int> counter{0};
    file = std::filesystem::temp_directory_path() / ("ontodb-semantic-" + std::to_string(::getpid()) + "-" + std::to_string(counter++) + ".db");
    disk = std::make_unique<DiskManager>(file.string());
    pool = std::make_unique<BufferPoolManager>(kPoolFrames, disk.get(), 2);
    hierarchy = std::make_unique<ClassHierarchy>(ClassHierarchy::Build({}));
    classes = std::make_unique<LabelIndex>(pool.get());
  }

  ~Impl() {
    search.reset();
    ivf.reset();
    classes.reset();
    pool.reset();
    disk.reset();
    std::error_code ignored;
    std::filesystem::remove(file, ignored);
  }

  auto Hybrid() -> HybridSearch& {
    if (search == nullptr) {
      search = std::make_unique<HybridSearch>(HybridSearch::Parts{hierarchy.get(), classes.get(), vectors.get(), flat.get(), ivf.get(), hnsw.get()});
    }
    return *search;
  }

  // The term a spec names: `<iri>` directly, otherwise the subject whose rdfs:label is the text.
  auto Resolve(TripleStore* store, Dictionary* dict, const std::string& raw) -> std::optional<term_id_t> {
    const std::string spec = Trim(raw);
    if (spec.empty()) {
      return std::nullopt;
    }
    if (spec[0] == '<') {
      return dict->Lookup(spec);
    }
    std::string lex = spec;
    if (lex.size() >= 2 && lex.front() == '"' && lex.back() == '"') {
      lex = lex.substr(1, lex.size() - 2);
    }
    const auto literal = dict->Lookup(CanonicalLiteral(lex, "", ""));
    const auto label = dict->Lookup(kLabel);
    if (!literal.has_value() || !label.has_value()) {
      return std::nullopt;
    }
    auto scan = store->Scan(TriplePattern{std::nullopt, *label, *literal});
    scan->Init();
    Triple t;
    return scan->Next(&t) ? std::optional<term_id_t>(t.subject) : std::nullopt;
  }

  auto Describe(TripleStore* store, Dictionary* dict, term_id_t id) -> std::string {
    std::string text = dict->Lookup(id).value_or("?");
    if (const auto label = dict->Lookup(kLabel); label.has_value()) {
      auto scan = store->Scan(TriplePattern{id, *label, std::nullopt});
      scan->Init();
      Triple t;
      if (scan->Next(&t)) {
        text += "  " + dict->Lookup(t.object).value_or("");
      }
    }
    return text;
  }
};

SemanticEngine::SemanticEngine() : impl_(std::make_unique<Impl>()) {}
SemanticEngine::~SemanticEngine() = default;

auto SemanticEngine::BuildHierarchy(TripleStore* store, Dictionary* dictionary, const std::string& predicate) -> std::string {
  try {
    const std::string spelling = Trim(predicate).empty() ? kSubClassOf : Trim(predicate);
    const auto id = dictionary->Lookup(spelling);
    if (!id.has_value()) {
      return "error: " + spelling + " is not in the dictionary (load an ontology first)";
    }
    const auto started = Clock::now();
    const auto edges = CollectSubClassEdges(store, *id);
    auto h = std::make_unique<ClassHierarchy>(ClassHierarchy::Build(edges));
    std::vector<std::pair<uint32_t, term_id_t>> pairs;
    std::vector<term_id_t> ids;
    for (const auto& e : edges) {
      ids.push_back(e.first);
      ids.push_back(e.second);
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    for (const term_id_t c : ids) {
      pairs.emplace_back(h->Label(c), c);
    }
    auto labels = std::make_unique<LabelIndex>(impl_->pool.get());
    labels->BulkLoad(std::move(pairs));
    impl_->search.reset();
    impl_->hierarchy = std::move(h);
    impl_->classes = std::move(labels);
    impl_->hierarchy_predicate = spelling;
    const auto& st = impl_->hierarchy->GetStats();
    std::ostringstream out;
    out << "labelled " << st.classes << " classes (" << st.components << " after collapsing cycles), " << st.edges << " edges, "
        << st.non_tree_edges << " outside the spanning forest; " << st.intervals << " intervals, at most " << st.max_intervals
        << " per class; " << Ms(started) << " ms";
    return out.str();
  } catch (const std::exception& e) {
    return std::string("error: ") + e.what();
  }
}

auto SemanticEngine::Under(TripleStore* store, Dictionary* dictionary, const std::string& cls, size_t limit) -> std::string {
  try {
    if (impl_->hierarchy->GetStats().classes == 0) {
      return "error: no hierarchy; run \\hindex build first";
    }
    const auto id = impl_->Resolve(store, dictionary, cls);
    if (!id.has_value()) {
      return "error: no term named " + Trim(cls);
    }
    if (!impl_->hierarchy->Contains(*id)) {
      return "error: " + Trim(cls) + " is not in the hierarchy";
    }
    LabelIndex::ScanStats scans;
    const auto started = Clock::now();
    const auto members = impl_->classes->Scan(impl_->hierarchy->Intervals(*id), &scans);
    const double ms = Ms(started);
    std::ostringstream out;
    out << members.size() << " classes at or below " << Trim(cls) << " (" << scans.scans << " range scans, " << ms << " ms)";
    size_t shown = 0;
    for (const term_id_t m : members) {
      if (shown++ >= limit) {
        out << "\n... " << members.size() - limit << " more";
        break;
      }
      out << "\n  " << impl_->Describe(store, dictionary, m);
    }
    return out.str();
  } catch (const std::exception& e) {
    return std::string("error: ") + e.what();
  }
}

auto SemanticEngine::LoadVectors(Dictionary* dictionary, const std::string& path) -> std::string {
  try {
    if (Trim(path).empty()) {
      return "error: usage: \\vload <file>";
    }
    const auto started = Clock::now();
    auto vectors = std::make_unique<VectorStore>(VectorStore::PeekDimension(Trim(path)));
    const auto stats = vectors->Load(Trim(path), dictionary);
    auto flat = std::make_unique<FlatIndex>(vectors.get(), FlatIndex::Precision::kFloat32);
    impl_->search.reset();
    impl_->ivf.reset();
    impl_->hnsw.reset();
    impl_->flat = std::move(flat);
    impl_->vectors = std::move(vectors);
    std::ostringstream out;
    out << "read " << stats.read << " vectors of " << impl_->vectors->Dim() << " floats, kept " << stats.stored << ", skipped "
        << stats.skipped_unknown << " whose term is not in the dictionary; " << Ms(started) << " ms. Search is exact until \\vindex build.";
    return out.str();
  } catch (const std::exception& e) {
    return std::string("error: ") + e.what();
  }
}

auto SemanticEngine::BuildIndex(const std::string& kind) -> std::string {
  try {
    if (impl_->vectors == nullptr) {
      return "error: no vectors; run \\vload <file> first";
    }
    const std::string which = Trim(kind).empty() ? "all" : Trim(kind);
    if (which != "ivf" && which != "hnsw" && which != "all") {
      return "error: usage: \\vindex build ivf|hnsw|all";
    }
    std::ostringstream out;
    if (which == "ivf" || which == "all") {
      const auto started = Clock::now();
      impl_->search.reset();
      impl_->ivf = std::make_unique<IvfIndex>(impl_->vectors.get(), impl_->pool.get(), IvfIndex::Options{});
      impl_->ivf->Build();
      out << "ivf: " << impl_->ivf->Lists() << " lists in a B+ tree, " << Ms(started) / 1000.0 << " s\n";
    }
    if (which == "hnsw" || which == "all") {
      const auto started = Clock::now();
      impl_->search.reset();
      impl_->hnsw = std::make_unique<HnswIndex>(impl_->vectors.get(), HnswIndex::Options{});
      impl_->hnsw->Build();
      out << "hnsw: M=16, " << impl_->hnsw->TopLayer() + 1 << " layers, " << Ms(started) / 1000.0 << " s\n";
    }
    std::string text = out.str();
    if (!text.empty() && text.back() == '\n') {
      text.pop_back();
    }
    return text;
  } catch (const std::exception& e) {
    return std::string("error: ") + e.what();
  }
}

auto SemanticEngine::Similar(TripleStore* store, Dictionary* dictionary, const std::string& args) -> std::string {
  try {
    if (impl_->vectors == nullptr) {
      return "error: no vectors; run \\vload <file> first";
    }
    const auto tokens = Tokens(args);
    if (tokens.empty()) {
      return "error: usage: \\similar <term> [k] [under <class>] [using <strategy>]";
    }
    // term: units up to a number, `under`, or `using`
    size_t i = 0;
    std::string term, under, using_;
    size_t k = kDefaultK;
    auto take_name = [&]() {
      std::string name;
      while (i < tokens.size() && !IsNumber(tokens[i]) && tokens[i] != "under" && tokens[i] != "using") {
        name += (name.empty() ? "" : " ") + tokens[i++];
        if (name.front() == '<' || name.front() == '"') {
          break;
        }
      }
      return name;
    };
    term = take_name();
    while (i < tokens.size()) {
      if (IsNumber(tokens[i])) {
        k = std::stoul(tokens[i++]);
      } else if (tokens[i] == "under") {
        i++;
        under = take_name();
      } else if (tokens[i] == "using") {
        i++;
        using_ = i < tokens.size() ? tokens[i++] : "";
      } else {
        return "error: unexpected " + tokens[i];
      }
    }
    if (k == 0) {
      return "error: k must be at least 1";
    }
    const auto id = impl_->Resolve(store, dictionary, term);
    if (!id.has_value()) {
      return "error: no term named " + term;
    }
    const auto row = impl_->vectors->Row(*id);
    if (!row.has_value()) {
      return "error: " + term + " has no vector";
    }
    term_id_t under_id = INVALID_TERM_ID;
    if (!under.empty()) {
      const auto cid = impl_->Resolve(store, dictionary, under);
      if (!cid.has_value()) {
        return "error: no term named " + under;
      }
      if (!impl_->hierarchy->Contains(*cid)) {
        return "error: " + under + " is not in the hierarchy (run \\hindex build)";
      }
      under_id = *cid;
    }
    using S = HybridSearch::Strategy;
    S strategy = S::kAuto;
    if (!using_.empty()) {
      if (using_ == "auto") strategy = S::kAuto;
      else if (using_ == "pre") strategy = S::kPreFilter;
      else if (using_ == "post-hnsw") strategy = S::kPostFilterHnsw;
      else if (using_ == "post-ivf") strategy = S::kPostFilterIvf;
      else if (using_ == "mask-hnsw") strategy = S::kMaskHnsw;
      else if (using_ == "mask-ivf") strategy = S::kMaskIvf;
      else return "error: unknown strategy " + using_;
      const bool needs_hnsw = strategy == S::kPostFilterHnsw || strategy == S::kMaskHnsw;
      const bool needs_ivf = strategy == S::kPostFilterIvf || strategy == S::kMaskIvf;
      if ((needs_hnsw && impl_->hnsw == nullptr) || (needs_ivf && impl_->ivf == nullptr)) {
        return "error: that index is not built; run \\vindex build";
      }
    }
    // One more than asked, because the term itself is its own nearest neighbor.
    const auto started = Clock::now();
    const auto result = impl_->Hybrid().Similar(impl_->vectors->Vector(*row), k + 1, under_id, strategy, HybridSearch::Params{});
    const double ms = Ms(started);
    std::ostringstream out;
    out << "nearest to " << impl_->Describe(store, dictionary, *id);
    if (under_id != INVALID_TERM_ID) {
      out << ", under " << under;
    }
    std::string used = HybridSearch::Name(result.used);
    if (under_id == INVALID_TERM_ID) {  // with no class there is no filter to name
      used = result.used == S::kMaskHnsw ? "hnsw" : result.used == S::kMaskIvf ? "ivf" : "exact scan";
    }
    out << " [" << used << ", " << ms * 1000.0 << " us]";
    size_t shown = 0;
    for (const Neighbor& n : result.neighbors) {
      if (n.id == *id) {
        continue;
      }
      if (shown++ >= k) {
        break;
      }
      char score[16];
      std::snprintf(score, sizeof score, "%.3f", n.score);
      out << "\n  " << score << "  " << impl_->Describe(store, dictionary, n.id);
    }
    if (shown == 0) {
      out << "\n  (none)";
    }
    return out.str();
  } catch (const std::exception& e) {
    return std::string("error: ") + e.what();
  }
}

auto SemanticEngine::Status() const -> std::string {
  std::ostringstream out;
  const auto& st = impl_->hierarchy->GetStats();
  if (st.classes == 0) {
    out << "hierarchy: not built (\\hindex build)\n";
  } else {
    out << "hierarchy: " << st.classes << " classes, " << st.intervals << " intervals (" << impl_->hierarchy_predicate << ")\n";
  }
  if (impl_->vectors == nullptr) {
    out << "vectors: none (\\vload <file>)";
  } else {
    out << "vectors: " << impl_->vectors->Size() << " of " << impl_->vectors->Dim() << " floats; flat"
        << (impl_->ivf ? ", ivf" : "") << (impl_->hnsw ? ", hnsw" : "");
  }
  return out.str();
}

}  // namespace ontodb
