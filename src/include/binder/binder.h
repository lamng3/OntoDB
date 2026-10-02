#pragma once

#include <string>
#include <unordered_map>

#include "binder/bound_query.h"
#include "dictionary/dictionary.h"
#include "parser/ast.h"

namespace ontodb {

// Resolves prefixes, interns constants, and assigns each variable a slot.
// Slots are assigned in order of first appearance. SELECT * projects every
// variable in that order. A select or ORDER BY variable that never appears
// in the pattern is an error.
class Binder {
 public:
  explicit Binder(Dictionary& dictionary);
  auto Bind(const AstQuery& query) -> BoundQuery;

 private:
  auto Intern(const std::string& canonical) -> term_id_t;
  auto BindTerm(const AstTerm& term, bool ground_only, bool intern) -> BoundTerm;
  auto BindFilter(const AstFilter& filter) -> std::unique_ptr<BoundFilter>;
  auto Slot(const std::string& name, int line, int column) -> size_t;

  Dictionary& dictionary_;
  std::unordered_map<std::string, std::string> prefixes_;
  std::unordered_map<std::string, size_t> slots_;
  std::vector<std::string> var_names_;
};

}  // namespace ontodb
