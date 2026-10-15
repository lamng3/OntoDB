#pragma once

#include <string>
#include <unordered_map>

#include "binder/bound_query.h"
#include "common/types.h"

namespace ontodb {

class Dictionary;

// = and != use numeric equality when both sides are numeric literals, and
// term identity otherwise. Ordered comparisons are numeric for numeric
// literals and lexicographic for two terms of the same kind. A comparison
// that SPARQL would treat as a type error is false.
auto CompareTerms(term_id_t left_id, term_id_t right_id, BoundFilter::Op op,
                  const Dictionary& dictionary) -> bool;

// A total order on terms for ORDER BY, after SPARQL 1.1 section 15.1:
// unbound, then blank nodes, then IRIs, then literals. Numeric literals come
// first among literals and compare by value; other literals compare by
// lexical form, then datatype, then language. Remaining ties go to the id, so
// the order is total. Without a dictionary, terms compare by id. Parsed terms
// are cached, so one TermOrder should serve a whole sort.
class TermOrder {
 public:
  explicit TermOrder(const Dictionary* dictionary);
  auto Compare(term_id_t left, term_id_t right) -> int;

 private:
  struct Key {
    int rank{0};  // 0 unbound, 1 blank, 2 IRI, 3 numeric literal, 4 other literal
    long double number{0};
    std::string lex;
    std::string datatype;
    std::string lang;
  };
  auto KeyOf(term_id_t id) -> const Key&;

  const Dictionary* dictionary_;
  std::unordered_map<term_id_t, Key> cache_;
};

}  // namespace ontodb
