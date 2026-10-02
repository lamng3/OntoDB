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

struct AstTriple {
  AstTerm subject;
  AstTerm predicate;
  AstTerm object;
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

struct AstQuery {
  enum class Kind { kSelect, kInsert, kDelete };
  Kind kind{Kind::kSelect};
  std::vector<std::pair<std::string, std::string>> prefixes;
  bool distinct{false};
  bool star{false};
  std::vector<std::string> select_vars;
  std::vector<AstTriple> triples;
  std::unique_ptr<AstFilter> filter;
  std::vector<AstOrderKey> order_by;
  std::optional<uint64_t> limit;
  std::optional<uint64_t> offset;
};

}  // namespace ontodb
