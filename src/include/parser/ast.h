#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ontodb {

struct AstTerm {
  enum class Kind { kVariable, kIri, kPrefixed, kLiteral, kBlank, kRdfType };
  Kind kind{Kind::kIri};
  std::string value;
  std::string extra;
  bool extra_is_prefixed{false};
  int line{1};
  int column{1};

  auto Text() const -> std::string;
};

// A property path. A single link is stored as the triple's predicate instead,
// so a basic graph pattern stays a list of triples.
struct AstPath {
  enum class Op { kLink, kInverse, kSeq, kAlt, kZeroOrMore, kOneOrMore, kZeroOrOne };
  Op op{Op::kLink};
  AstTerm link;
  std::vector<std::unique_ptr<AstPath>> parts;
};

struct AstTriple {
  AstTerm subject;
  AstTerm predicate;
  AstTerm object;
  std::unique_ptr<AstPath> path;
};

struct AstFilter {
  enum class Op { kEq, kNe, kLt, kGt, kLe, kGe, kAnd };
  Op op{Op::kEq};
  AstTerm left;
  AstTerm right;
  std::unique_ptr<AstFilter> lhs;
  std::unique_ptr<AstFilter> rhs;

  static auto Compare(Op op, AstTerm left, AstTerm right) -> std::unique_ptr<AstFilter>;
  static auto And(std::unique_ptr<AstFilter> lhs,
                  std::unique_ptr<AstFilter> rhs) -> std::unique_ptr<AstFilter>;
  auto Text() const -> std::string;
};

struct AstOrderKey {
  std::string variable;
  bool ascending{true};
  int line{1};
  int column{1};
};

// One piece of a group graph pattern. A flat basic graph pattern is not stored
// here: the parser copies it into AstQuery::triples so the rest of the engine
// is unchanged. This tree is set when the group is not flat.
struct AstPattern {
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
  std::vector<std::unique_ptr<AstPattern>> children;
  std::vector<AstTriple> triples;
  std::unique_ptr<AstFilter> filter;
  std::string bind_var;
  AstTerm bind_expr;
  std::vector<std::string> values_vars;
  // A null term is UNDEF.
  std::vector<std::vector<std::optional<AstTerm>>> values_rows;
  AstTerm graph;
  bool graph_is_var{false};
  bool silent{false};
  int line{1};
  int column{1};
};

struct AstAggregate {
  enum class Op { kCount, kSum, kAvg, kMin, kMax, kSample };
  Op op{Op::kCount};
  bool distinct{false};
  bool star{false};
  AstTerm arg;
  std::string alias;
  int line{1};
  int column{1};
};

struct AstQuery {
  enum class Kind { kSelect, kInsert, kDelete };
  Kind kind{Kind::kSelect};
  std::vector<std::pair<std::string, std::string>> prefixes;
  bool distinct{false};
  bool reduced{false};  // SPARQL allows REDUCED to keep every row
  bool star{false};
  std::vector<std::string> select_vars;
  std::vector<std::string> projection_order;  // vars and aggregate aliases, in written order
  std::vector<AstAggregate> aggregates;
  std::vector<std::string> group_by;
  std::unique_ptr<AstFilter> having;
  std::vector<AstTriple> triples;
  std::unique_ptr<AstFilter> filter;
  std::unique_ptr<AstPattern> pattern;  // set when the WHERE group is not a flat pattern
  // DELETE/INSERT WHERE. The templates may hold variables. `triples` is unused.
  bool update_where{false};
  std::vector<AstTriple> delete_template;
  std::vector<AstTriple> insert_template;
  std::vector<AstOrderKey> order_by;
  std::optional<uint64_t> limit;
  std::optional<uint64_t> offset;
};

}  // namespace ontodb
