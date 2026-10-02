#pragma once

#include <string>
#include <string_view>

namespace ontodb {

enum class TokenKind {
  kEof,
  kError,
  kPrefix,
  kSelect,
  kDistinct,
  kWhere,
  kFilter,
  kInsert,
  kDelete,
  kData,
  kOrder,
  kBy,
  kLimit,
  kOffset,
  kAsc,
  kDesc,
  kOptional,
  kUnion,
  kMinus,
  kBind,
  kValues,
  kGraph,
  kService,
  kConstruct,
  kAsk,
  kDescribe,
  kGroup,
  kHaving,
  kReduced,
  kCount,
  kSum,
  kAvg,
  kMin,
  kMax,
  kSample,
  kBase,
  kA,
  kDot,
  kSemicolon,
  kComma,
  kLBrace,
  kRBrace,
  kLParen,
  kRParen,
  kLBracket,
  kStar,
  kPlus,
  kPipe,
  kSlash,
  kAndAnd,
  kEq,
  kNe,
  kLt,
  kGt,
  kLe,
  kGe,
  kHatHat,
  kIri,
  kPrefixed,
  kVar,
  kString,
  kInteger,
  kDecimal,
  kLang,
  kBlank,
  kIdent
};

struct Token {
  TokenKind kind{TokenKind::kEof};
  std::string text;
  int line{1};
  int column{1};
};

class Lexer {
 public:
  explicit Lexer(std::string_view input);
  auto Peek() -> const Token&;
  auto Next() -> Token;

 private:
  auto Scan() -> Token;
  auto StartsIri(size_t at) const -> bool;

  std::string_view input_;
  size_t pos_{0};
  int line_{1};
  int column_{1};
  Token peeked_{};
  bool has_peek_{false};
};

auto TokenName(TokenKind kind) -> const char*;

}  // namespace ontodb
