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
};

}  // namespace ontodb
