// The Python extension: the engine's C++ API, as values. The public API is python/ontodb.
#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "common/database.h"
#include "common/session_state.h"
#include "shell/shell.h"

namespace nb = nanobind;

namespace {

using Vectors = nb::ndarray<const float, nb::ndim<2>, nb::c_contig, nb::device::cpu>;
using Query = nb::ndarray<const float, nb::ndim<1>, nb::c_contig, nb::device::cpu>;
using Hits = std::vector<std::pair<std::string, float>>;

auto ToPairs(const std::vector<ontodb::SemanticHit>& hits) -> Hits {
  Hits out;
  out.reserve(hits.size());
  for (const auto& hit : hits) {
    out.emplace_back(hit.term, hit.score);
  }
  return out;
}

// The engine reports some failures as a line of text beginning "error: ".
auto Checked(std::string text) -> std::string {
  if (text.rfind("error: ", 0) == 0) {
    throw std::runtime_error(text.substr(7));
  }
  return text;
}

class Connection {
 public:
  Connection(const std::optional<std::string>& path, bool strict) {
    if (path.has_value()) {
      const auto mode =
          strict ? ontodb::RecoveryMode::kAbsoluteConsistency : ontodb::RecoveryMode::kPointInTime;
      db_ = ontodb::Database::Open(*path, mode);
    } else {
      db_ = std::make_unique<ontodb::Database>();
    }
  }

  void Close() {
    const std::lock_guard<std::mutex> guard(mu_);
    db_.reset();
  }

  auto Closed() const -> bool { return db_ == nullptr; }
  auto Durable() -> bool { return Db().IsDurable(); }

  auto Load(const std::string& path) -> std::pair<uint64_t, double> {
    const auto stats = Db().Load(path);
    return {stats.triples, stats.milliseconds};
  }

  auto Execute(const std::string& sparql) -> ontodb::StatementResult {
    return Db().Execute(sparql, &session_);
  }

  void Begin() { Db().Begin(); }
  void Commit() { Db().Commit(); }
  void Abort() { Db().Abort(); }
  auto Checkpoint() -> std::string { return Db().Checkpoint(); }
  auto Verify() -> std::string { return Db().Verify(); }

  auto BuildHierarchy(const std::string& predicate) -> std::string {
    const std::lock_guard<std::mutex> guard(mu_);
    auto& db = Db();
    return Checked(db.Semantic().BuildHierarchy(db.GetStore(), db.GetDictionary(), predicate));
  }

  auto Under(const std::string& cls, size_t limit) -> std::vector<std::string> {
    const std::lock_guard<std::mutex> guard(mu_);
    auto& db = Db();
    return db.Semantic().UnderTerms(db.GetStore(), db.GetDictionary(), cls, limit);
  }

  auto PutVectors(const std::vector<std::string>& terms, const float* data,
                  uint32_t dim) -> size_t {
    const std::lock_guard<std::mutex> guard(mu_);
    auto& db = Db();
    return db.Semantic().PutVectors(db.GetDictionary(), terms, data, dim);
  }

  auto BuildIndex(const std::string& kind) -> std::string {
    const std::lock_guard<std::mutex> guard(mu_);
    return Checked(Db().Semantic().BuildIndex(kind));
  }

  auto Nearest(const float* query, uint32_t dim, size_t k, const std::string& under) -> Hits {
    const std::lock_guard<std::mutex> guard(mu_);
    auto& db = Db();
    return ToPairs(db.Semantic().Nearest(db.GetStore(), db.GetDictionary(), query, dim, k, under));
  }

  auto NearestTerm(const std::string& term, size_t k, const std::string& under) -> Hits {
    const std::lock_guard<std::mutex> guard(mu_);
    auto& db = Db();
    return ToPairs(db.Semantic().NearestTerm(db.GetStore(), db.GetDictionary(), term, k, under));
  }

 private:
  auto Db() -> ontodb::Database& {
    if (db_ == nullptr) {
      throw std::runtime_error("the connection is closed");
    }
    return *db_;
  }

  std::unique_ptr<ontodb::Database> db_;
  ontodb::SessionState session_;
  std::mutex mu_;  // the semantic engine is not thread-safe
};

void RunShell(const std::optional<std::string>& path, bool strict) {
  std::unique_ptr<ontodb::Database> db;
  if (path.has_value()) {
    ontodb::OpenReport report;
    const auto mode =
        strict ? ontodb::RecoveryMode::kAbsoluteConsistency : ontodb::RecoveryMode::kPointInTime;
    db = ontodb::Database::Open(*path, mode, &report);
    std::cout << report.ToString() << "\n";
  } else {
    db = std::make_unique<ontodb::Database>();
  }
  ontodb::Shell shell(*db, std::cin, std::cout);
  shell.Run();
}

}  // namespace

NB_MODULE(_ontodb, m) {
  nb::class_<ontodb::StatementResult>(m, "StatementResult")
      .def_ro("is_query", &ontodb::StatementResult::is_query)
      .def_ro("columns", &ontodb::StatementResult::columns)
      .def_ro("rows", &ontodb::StatementResult::rows)
      .def_ro("message", &ontodb::StatementResult::message)
      .def_ro("milliseconds", &ontodb::StatementResult::milliseconds);

  using Release = nb::call_guard<nb::gil_scoped_release>;
  nb::class_<Connection>(m, "Connection")
      .def(nb::init<const std::optional<std::string>&, bool>(), nb::arg("path").none(),
           nb::arg("strict"), Release())
      .def("close", &Connection::Close, Release())
      .def_prop_ro("closed", &Connection::Closed)
      .def_prop_ro("durable", &Connection::Durable)
      .def("load", &Connection::Load, Release())
      .def("execute", &Connection::Execute, Release())
      .def("begin", &Connection::Begin, Release())
      .def("commit", &Connection::Commit, Release())
      .def("abort", &Connection::Abort, Release())
      .def("checkpoint", &Connection::Checkpoint, Release())
      .def("verify", &Connection::Verify, Release())
      .def("build_hierarchy", &Connection::BuildHierarchy, Release())
      .def("under", &Connection::Under, Release())
      .def("build_index", &Connection::BuildIndex, Release())
      .def("nearest_term", &Connection::NearestTerm, Release())
      .def("put_vectors",
           [](Connection& self, const std::vector<std::string>& terms, Vectors vectors) {
             if (vectors.shape(0) != terms.size()) {
               throw std::invalid_argument("one row of vectors is needed per term");
             }
             const auto dim = static_cast<uint32_t>(vectors.shape(1));
             const nb::gil_scoped_release release;
             return self.PutVectors(terms, vectors.data(), dim);
           })
      .def("nearest", [](Connection& self, Query query, size_t k, const std::string& under) {
        const auto dim = static_cast<uint32_t>(query.shape(0));
        const nb::gil_scoped_release release;
        return self.Nearest(query.data(), dim, k, under);
      });

  m.def("run_shell", &RunShell, nb::arg("path").none(), nb::arg("strict"), Release());
}
