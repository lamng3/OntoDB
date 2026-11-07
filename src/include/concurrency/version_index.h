#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <vector>

#include "common/types.h"

namespace ontodb {

using commit_ts_t = uint64_t;

// A span that has not ended: the triple is in the latest version.
inline constexpr commit_ts_t kOpenEnd = static_cast<commit_ts_t>(-1);

// The commits in which a triple was present: begin <= ts < end.
struct VersionSpan {
  commit_ts_t begin{0};
  commit_ts_t end{kOpenEnd};

  friend auto operator==(const VersionSpan&, const VersionSpan&) -> bool = default;
};

struct VersionedTriple {
  Triple triple;
  VersionSpan span;
};

struct VersionChange {
  bool is_insert{true};
  Triple triple;
};

struct VersionTag {
  std::string name;
  commit_ts_t ts{0};
};

// Every committed state of the database, beside the store the locks protect.
//
// Each committed transaction that changed something gets the next timestamp. A triple is stored
// once, with the spans in which it was present, so versions share every triple they have in common
// and a version costs only what it changed. Reading at a timestamp takes no transaction lock, and
// writers never change what an earlier timestamp sees. The mutex is a latch held for one call: a
// scan copies its matches out, so a slow reader cannot hold up a commit.
//
// Three orderings serve any pattern by a range scan: (s p o), (p o s), and (o s p).
//
// A tag names a timestamp, for good: it can never move to another, and a name given to the
// timestamp it already has is not a change.
//
// History is kept until the database is deleted; nothing vacuums it. An ontology's history is small
// next to its value, but a workload of many updates would want a retention policy.
class VersionIndex {
 public:
  // Commits one transaction's changes, in the order they were made, and returns its timestamp.
  // With no changes nothing commits, and the result is the current timestamp.
  auto Apply(const std::vector<VersionChange>& changes) -> commit_ts_t;

  // The newest committed timestamp; 0 before the first commit.
  auto Clock() const -> commit_ts_t;

  auto Scan(const TriplePattern& pattern, commit_ts_t at) const -> std::vector<Triple>;
  auto Contains(const Triple& triple, commit_ts_t at) const -> bool;
  auto Count(commit_ts_t at) const -> size_t;
  // The timestamp of the last commit to add or remove the triple; 0 if none ever did.
  auto LastChange(const Triple& triple) const -> commit_ts_t;

  // Names the current timestamp, or `ts` when given. Throws std::invalid_argument when the name
  // holds whitespace or is empty, or is already another timestamp's, or `ts` is in the future.
  auto Tag(const std::string& name, std::optional<commit_ts_t> ts = std::nullopt) -> commit_ts_t;
  auto FindTag(const std::string& name) const -> std::optional<commit_ts_t>;
  auto Tags() const -> std::vector<VersionTag>;  // oldest first

  // The whole history, to write a snapshot or compare two indexes, sorted by triple then span.
  auto Dump() const -> std::vector<VersionedTriple>;
  // Replaces everything; for an index being rebuilt from a snapshot.
  void Restore(const std::vector<VersionedTriple>& spans, const std::vector<VersionTag>& tags,
               commit_ts_t clock);
  // Gives every triple new ids, for a backend switch, where the dictionary's ids change.
  void Remap(const std::function<Triple(const Triple&)>& to);

  // How many triples have ever existed, and how many spans they have.
  auto TripleCount() const -> size_t;
  auto SpanCount() const -> size_t;

 private:
  void Add(const Triple& triple, std::vector<VersionSpan> spans);

  mutable std::shared_mutex mu_;
  std::map<Triple, std::vector<VersionSpan>> spo_;
  std::set<Triple> pos_;  // as (p, o, s)
  std::set<Triple> osp_;  // as (o, s, p)
  std::map<std::string, commit_ts_t> tags_;
  commit_ts_t clock_{0};
};

}  // namespace ontodb
