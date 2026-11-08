#pragma once

#include <string_view>

#include "parser/ast.h"
#include "parser/lexer.h"

namespace ontodb {

// Recursive-descent parser. A flat basic graph pattern is stored as triples.
// OPTIONAL, UNION, MINUS, BIND, VALUES, GRAPH, SERVICE, property paths, nested
// groups, blank-node lists, aggregates, and DELETE/INSERT WHERE are stored on
// the query and executed. A parse error still names its line and column.
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
  void InstallPattern(AstQuery* query, std::unique_ptr<AstPattern> pattern);
  auto ParseBraced() -> std::unique_ptr<AstPattern>;
  auto ParseGroupBody() -> std::unique_ptr<AstPattern>;
  auto ParseTriplesBlock() -> std::unique_ptr<AstPattern>;
  void ParseTriplesSameSubject(std::vector<AstTriple>* out);
  auto ParsePath() -> std::unique_ptr<AstPath>;
  auto ParsePathSeq() -> std::unique_ptr<AstPath>;
  auto ParsePathElt() -> std::unique_ptr<AstPath>;
  auto ParseVerb() -> AstTerm;
  auto ParseObjectList() -> std::vector<AstTerm>;
  auto ParseTerm() -> AstTerm;
  auto ParseBlankPropertyList() -> AstTerm;
  auto ParseCollection() -> AstTerm;
  auto FreshBlank() -> AstTerm;
  auto ParseFilter() -> std::unique_ptr<AstFilter>;
  auto ParseFilterAnd() -> std::unique_ptr<AstFilter>;
  auto ParseFilterPrimary() -> std::unique_ptr<AstFilter>;
  auto ParseAggregate() -> AstAggregate;
  void ParseSolutionModifier(AstQuery* query);
  auto ParseInteger() -> uint64_t;
  auto ParseValues() -> std::unique_ptr<AstPattern>;

  Lexer lexer_;
  int blanks_{0};
  std::vector<AstTriple>* sink_{nullptr};
};

}  // namespace ontodb
