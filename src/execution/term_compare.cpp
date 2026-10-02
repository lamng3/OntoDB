#include "execution/term_compare.h"

#include "common/term_codec.h"
#include "dictionary/dictionary.h"

namespace ontodb {
namespace {

auto Numeric(const ParsedTerm& term) -> std::optional<long double> {
  if (term.kind != ParsedTerm::Kind::kLiteral || !IsNumericDatatype(term.datatype)) {
    return std::nullopt;
  }
  try {
    size_t consumed = 0;
    const long double value = std::stold(term.value, &consumed);
    if (consumed != term.value.size()) {
      return std::nullopt;
    }
    return value;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

auto Lex(const ParsedTerm& term) -> std::string {
  if (term.kind == ParsedTerm::Kind::kLiteral) {
    return term.value;
  }
  return term.value;
}

}  // namespace

auto CompareTerms(term_id_t left_id, term_id_t right_id, BoundFilter::Op op,
                  const Dictionary& dictionary) -> bool {
  if (op == BoundFilter::Op::kEq || op == BoundFilter::Op::kNe) {
    const auto left_text = dictionary.Lookup(left_id);
    const auto right_text = dictionary.Lookup(right_id);
    if (left_text && right_text) {
      const auto left = ParseCanonical(*left_text);
      const auto right = ParseCanonical(*right_text);
      const auto ln = Numeric(left);
      const auto rn = Numeric(right);
      if (ln && rn) {
        const bool equal = *ln == *rn;
        return op == BoundFilter::Op::kEq ? equal : !equal;
      }
    }
    const bool equal = left_id == right_id;
    return op == BoundFilter::Op::kEq ? equal : !equal;
  }

  const auto left_text = dictionary.Lookup(left_id);
  const auto right_text = dictionary.Lookup(right_id);
  if (!left_text || !right_text) {
    return false;
  }
  const auto left = ParseCanonical(*left_text);
  const auto right = ParseCanonical(*right_text);
  int cmp = 0;
  if (const auto ln = Numeric(left); ln) {
    if (const auto rn = Numeric(right); rn) {
      cmp = (*ln < *rn) ? -1 : (*ln > *rn ? 1 : 0);
    } else {
      return false;
    }
  } else if (left.kind == right.kind && left.datatype == right.datatype &&
             left.lang == right.lang) {
    const auto ls = Lex(left);
    const auto rs = Lex(right);
    cmp = (ls < rs) ? -1 : (ls > rs ? 1 : 0);
  } else {
    return false;
  }
  switch (op) {
    case BoundFilter::Op::kLt:
      return cmp < 0;
    case BoundFilter::Op::kGt:
      return cmp > 0;
    case BoundFilter::Op::kLe:
      return cmp <= 0;
    case BoundFilter::Op::kGe:
      return cmp >= 0;
    default:
      return false;
  }
}

}  // namespace ontodb
