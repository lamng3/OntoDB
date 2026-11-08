#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/types.h"

namespace ontodb {

struct BoundTerm {
  bool is_variable{false};
  size_t slot{0};
  term_id_t id{INVALID_TERM_ID};
  std::string text;
};

struct BoundTriplePattern {
  BoundTerm subject;
  BoundTerm predicate;
  BoundTerm object;
  std::string text;
  // Set by the entailment rewrite: this `(x rdf:type C)` scan returns the instances of C and of
  // its subclasses, and is expected to return `entailed_rows` of them.
  bool entailed{false};
  double entailed_rows{0};
};

struct BoundFilter {
  enum class Op { kEq, kNe, kLt, kGt, kLe, kGe, kAnd };
  Op op{Op::kEq};
  BoundTerm left;
  BoundTerm right;
  std::unique_ptr<BoundFilter> lhs;
  std::unique_ptr<BoundFilter> rhs;
  std::string text;
};

struct BoundPath {
  enum class Op { kLink, kInverse, kSeq, kAlt, kZeroOrMore, kOneOrMore, kZeroOrOne };
  Op op{Op::kLink};
  BoundTerm link;
  std::vector<std::unique_ptr<BoundPath>> parts;
};

// A group that is not a flat basic graph pattern: OPTIONAL, UNION, MINUS,
// BIND, VALUES, GRAPH, SERVICE, nested groups, or a property path.
struct BoundPattern {
  enum class Kind {
    kJoin,
    kOptional,
    kUnion,
    kMinus,
    kFilter,
    kBind,
    kValues,
    kGraph,
    kService,
    kTriples
  };
  Kind kind{Kind::kJoin};
  std::vector<std::unique_ptr<BoundPattern>> children;
  std::vector<BoundTriplePattern> triples;
  std::vector<std::unique_ptr<BoundPath>> paths;  // aligned with triples; null means a plain triple
  std::unique_ptr<BoundFilter> filter;
  size_t bind_slot{0};
  BoundTerm bind_expr;
  std::vector<size_t> values_slots;
  std::vector<std::vector<term_id_t>> values_rows;  // INVALID_TERM_ID is UNDEF
  BoundTerm graph;
  bool graph_is_var{false};
  size_t graph_slot{0};
  bool silent{false};
  std::string label;
};

struct BoundAggregate {
  enum class Op { kCount, kSum, kAvg, kMin, kMax, kSample };
  Op op{Op::kCount};
  bool distinct{false};
  bool star{false};
  size_t arg{0};
  std::string alias;
};

struct BoundOrderKey {
  size_t slot{0};
  bool ascending{true};
  std::string name;
};

struct BoundQuery {
  enum class Kind { kSelect, kInsert, kDelete };
  Kind kind{Kind::kSelect};
  bool distinct{false};
  bool star{false};
  std::vector<std::string> var_names;
  std::vector<std::string> projection_names;
  std::vector<size_t> projection_slots;
  std::vector<BoundTriplePattern> patterns;
  std::unique_ptr<BoundFilter> filter;
  std::vector<BoundOrderKey> order_by;
  std::optional<uint64_t> limit;
  std::optional<uint64_t> offset;
  std::vector<Triple> ground_triples;
  std::shared_ptr<BoundPattern> pattern;
  std::vector<BoundAggregate> aggregates;
  std::vector<size_t> group_slots;
  std::unique_ptr<BoundFilter> having;
  bool update_where{false};
  std::vector<BoundTriplePattern> delete_template;
  std::vector<BoundTriplePattern> insert_template;
};

}  // namespace ontodb
