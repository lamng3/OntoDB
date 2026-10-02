#pragma once

#include <string_view>

#include "parser/ast.h"
#include "parser/lexer.h"

namespace ontodb {

// Recursive-descent parser for the read subset plus INSERT DATA and DELETE DATA.
// OPTIONAL, UNION, property paths, aggregates, and DELETE/INSERT WHERE throw
// ParseException with a line and column.
class Parser {
 public:
  explicit Parser(std::string_view input);
  auto Parse() -> AstQuery;

 private:
  auto Peek() -> const Token&;
  auto Next() -> Token;
  auto Match(TokenKind kind) -> bool;
  auto Expect(TokenKind kind, const char* what) -> Token;
  void Unsupported(const Token& token, const char* feature);

  void ParsePrologue(AstQuery* query);
  void ParseSelect(AstQuery* query);
  void ParseUpdate(AstQuery* query, AstQuery::Kind kind);
  void ParseGroup(AstQuery* query);
  void ParseTriplesSameSubject(AstQuery* query);
  auto ParseVerb() -> AstTerm;
  auto ParseObjectList() -> std::vector<AstTerm>;
  auto ParseTerm() -> AstTerm;
  auto ParseFilter() -> std::unique_ptr<AstFilter>;
  auto ParseFilterAnd() -> std::unique_ptr<AstFilter>;
  auto ParseFilterPrimary() -> std::unique_ptr<AstFilter>;
  void ParseSolutionModifier(AstQuery* query);
  auto ParseInteger() -> uint64_t;

  Lexer lexer_;
};

}  // namespace ontodb
