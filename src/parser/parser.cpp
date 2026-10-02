#include "parser/parser.h"

#include <utility>

#include "common/config.h"
#include "common/exception.h"
#include "common/term_codec.h"

namespace ontodb {
namespace {

auto SplitPrefixed(const std::string& text) -> std::pair<std::string, std::string> {
  const auto pos = text.find('\n');
  if (pos == std::string::npos) {
    return {text, {}};
  }
  return {text.substr(0, pos), text.substr(pos + 1)};
}

}  // namespace

Parser::Parser(std::string_view input) : lexer_(input) {}

auto Parser::Peek() -> const Token& {
  return lexer_.Peek();
}

auto Parser::Next() -> Token {
  return lexer_.Next();
}

auto Parser::Match(TokenKind kind) -> bool {
  if (Peek().kind != kind) {
    return false;
  }
  Next();
  return true;
}

auto Parser::Expect(TokenKind kind, const char* what) -> Token {
  if (Peek().kind != kind) {
    throw ParseException(Peek().line, Peek().column, std::string("expected ") + what);
  }
  return Next();
}

void Parser::Unsupported(const Token& token, const char* feature) {
  throw ParseException(token.line, token.column, std::string(feature) + " is not supported");
}

auto Parser::Parse() -> AstQuery {
  AstQuery query;
  ParsePrologue(&query);
  if (Peek().kind == TokenKind::kEof) {
    throw ParseException(Peek().line, Peek().column, "expected SELECT, INSERT, or DELETE");
  }
  if (Peek().kind == TokenKind::kConstruct) {
    Unsupported(Peek(), "CONSTRUCT");
  }
  if (Peek().kind == TokenKind::kAsk) {
    Unsupported(Peek(), "ASK");
  }
  if (Peek().kind == TokenKind::kDescribe) {
    Unsupported(Peek(), "DESCRIBE");
  }
  if (Match(TokenKind::kSelect)) {
    ParseSelect(&query);
  } else if (Match(TokenKind::kInsert)) {
    ParseUpdate(&query, AstQuery::Kind::kInsert);
  } else if (Match(TokenKind::kDelete)) {
    ParseUpdate(&query, AstQuery::Kind::kDelete);
  } else {
    throw ParseException(Peek().line, Peek().column, "expected SELECT, INSERT, or DELETE");
  }
  if (Peek().kind != TokenKind::kEof) {
    throw ParseException(Peek().line, Peek().column, "unexpected input after the query");
  }
  return query;
}

void Parser::ParsePrologue(AstQuery* query) {
  while (Match(TokenKind::kPrefix)) {
    const Token name = Expect(TokenKind::kPrefixed, "prefix name");
    const auto parts = SplitPrefixed(name.text);
    if (!parts.second.empty()) {
      throw ParseException(name.line, name.column, "prefix declaration is missing a colon");
    }
    const Token iri = Expect(TokenKind::kIri, "prefix IRI");
    query->prefixes.emplace_back(parts.first, iri.text);
  }
  if (Peek().kind == TokenKind::kBase) {
    Unsupported(Peek(), "BASE");
  }
}

void Parser::ParseSelect(AstQuery* query) {
  query->kind = AstQuery::Kind::kSelect;
  if (Match(TokenKind::kDistinct)) {
    query->distinct = true;
  } else if (Peek().kind == TokenKind::kReduced) {
    Unsupported(Peek(), "REDUCED");
  }
  if (Match(TokenKind::kStar)) {
    query->star = true;
  } else if (Peek().kind == TokenKind::kLParen || Peek().kind == TokenKind::kCount ||
             Peek().kind == TokenKind::kSum || Peek().kind == TokenKind::kAvg ||
             Peek().kind == TokenKind::kMin || Peek().kind == TokenKind::kMax ||
             Peek().kind == TokenKind::kSample) {
    Unsupported(Peek(), "aggregates");
  } else {
    while (Peek().kind == TokenKind::kVar) {
      query->select_vars.push_back(Next().text);
      Match(TokenKind::kComma);
    }
    if (query->select_vars.empty()) {
      throw ParseException(Peek().line, Peek().column, "expected select variables or *");
    }
  }
  Match(TokenKind::kWhere);
  Expect(TokenKind::kLBrace, "'{'");
  ParseGroup(query);
  Expect(TokenKind::kRBrace, "'}'");
  ParseSolutionModifier(query);
}

void Parser::ParseUpdate(AstQuery* query, AstQuery::Kind kind) {
  query->kind = kind;
  if (!Match(TokenKind::kData)) {
    Unsupported(Peek(), "DELETE/INSERT WHERE");
  }
  Expect(TokenKind::kLBrace, "'{'");
  ParseGroup(query);
  Expect(TokenKind::kRBrace, "'}'");
}

void Parser::ParseGroup(AstQuery* query) {
  while (Peek().kind != TokenKind::kRBrace && Peek().kind != TokenKind::kEof) {
    if (Peek().kind == TokenKind::kFilter) {
      auto filter = ParseFilter();
      if (query->filter == nullptr) {
        query->filter = std::move(filter);
      } else {
        query->filter = AstFilter::And(std::move(query->filter), std::move(filter));
      }
      Match(TokenKind::kDot);
      continue;
    }
    if (Peek().kind == TokenKind::kOptional) {
      Unsupported(Peek(), "OPTIONAL");
    }
    if (Peek().kind == TokenKind::kUnion) {
      Unsupported(Peek(), "UNION");
    }
    if (Peek().kind == TokenKind::kMinus) {
      Unsupported(Peek(), "MINUS");
    }
    if (Peek().kind == TokenKind::kBind) {
      Unsupported(Peek(), "BIND");
    }
    if (Peek().kind == TokenKind::kValues) {
      Unsupported(Peek(), "VALUES");
    }
    if (Peek().kind == TokenKind::kGraph) {
      Unsupported(Peek(), "GRAPH");
    }
    if (Peek().kind == TokenKind::kService) {
      Unsupported(Peek(), "SERVICE");
    }
    if (Peek().kind == TokenKind::kLBrace) {
      Unsupported(Peek(), "nested groups");
    }
    ParseTriplesSameSubject(query);
    while (Match(TokenKind::kDot)) {
      if (Peek().kind == TokenKind::kRBrace || Peek().kind == TokenKind::kFilter ||
          Peek().kind == TokenKind::kEof) {
        break;
      }
      if (Peek().kind == TokenKind::kOptional || Peek().kind == TokenKind::kUnion) {
        break;
      }
      ParseTriplesSameSubject(query);
    }
  }
}

void Parser::ParseTriplesSameSubject(AstQuery* query) {
  if (Peek().kind == TokenKind::kLBracket || Peek().kind == TokenKind::kLParen) {
    Unsupported(Peek(), "blank node property lists and collections");
  }
  const AstTerm subject = ParseTerm();
  bool any = false;
  while (true) {
    if (Peek().kind == TokenKind::kStar || Peek().kind == TokenKind::kPlus ||
        Peek().kind == TokenKind::kSlash || Peek().kind == TokenKind::kPipe) {
      Unsupported(Peek(), "property paths");
    }
    AstTerm predicate = ParseVerb();
    if (Peek().kind == TokenKind::kStar || Peek().kind == TokenKind::kPlus ||
        Peek().kind == TokenKind::kSlash || Peek().kind == TokenKind::kPipe) {
      Unsupported(Peek(), "property paths");
    }
    const std::vector<AstTerm> objects = ParseObjectList();
    for (const auto& object : objects) {
      query->triples.push_back(AstTriple{subject, predicate, object});
    }
    any = true;
    if (!Match(TokenKind::kSemicolon)) {
      break;
    }
    if (Peek().kind == TokenKind::kDot || Peek().kind == TokenKind::kRBrace ||
        Peek().kind == TokenKind::kFilter || Peek().kind == TokenKind::kEof) {
      break;
    }
  }
  if (!any) {
    throw ParseException(Peek().line, Peek().column, "expected a predicate");
  }
}

auto Parser::ParseVerb() -> AstTerm {
  if (Peek().kind == TokenKind::kA) {
    const Token token = Next();
    AstTerm term;
    term.kind = AstTerm::Kind::kRdfType;
    term.value = kRdfType;
    term.line = token.line;
    term.column = token.column;
    return term;
  }
  return ParseTerm();
}

auto Parser::ParseObjectList() -> std::vector<AstTerm> {
  std::vector<AstTerm> objects;
  objects.push_back(ParseTerm());
  while (Match(TokenKind::kComma)) {
    objects.push_back(ParseTerm());
  }
  return objects;
}

auto Parser::ParseTerm() -> AstTerm {
  const Token token = Peek();
  AstTerm term;
  term.line = token.line;
  term.column = token.column;
  if (token.kind == TokenKind::kVar) {
    Next();
    term.kind = AstTerm::Kind::kVariable;
    term.value = token.text;
    return term;
  }
  if (token.kind == TokenKind::kIri) {
    Next();
    term.kind = AstTerm::Kind::kIri;
    term.value = token.text;
    return term;
  }
  if (token.kind == TokenKind::kPrefixed) {
    Next();
    term.kind = AstTerm::Kind::kPrefixed;
    const auto parts = SplitPrefixed(token.text);
    term.value = parts.first;
    term.extra = parts.second;
    return term;
  }
  if (token.kind == TokenKind::kBlank) {
    Next();
    term.kind = AstTerm::Kind::kBlank;
    term.value = token.text;
    return term;
  }
  if (token.kind == TokenKind::kString) {
    Next();
    term.kind = AstTerm::Kind::kLiteral;
    term.value = UnescapeLiteral(token.text);
    if (Peek().kind == TokenKind::kLang) {
      term.extra = "@" + Next().text;
    } else if (Match(TokenKind::kHatHat)) {
      if (Peek().kind == TokenKind::kIri) {
        term.extra = CanonicalIri(Next().text);
      } else if (Peek().kind == TokenKind::kPrefixed) {
        term.extra_is_prefixed = true;
        const Token dt = Next();
        const auto parts = SplitPrefixed(dt.text);
        term.extra = parts.first + ":" + parts.second;
      } else {
        throw ParseException(Peek().line, Peek().column, "expected a datatype IRI");
      }
    }
    return term;
  }
  if (token.kind == TokenKind::kInteger) {
    Next();
    term.kind = AstTerm::Kind::kLiteral;
    term.value = token.text;
    term.extra = CanonicalIri(kXsdInteger);
    return term;
  }
  if (token.kind == TokenKind::kDecimal) {
    Next();
    term.kind = AstTerm::Kind::kLiteral;
    term.value = token.text;
    term.extra = CanonicalIri(kXsdDecimal);
    return term;
  }
  if (token.kind == TokenKind::kA) {
    throw ParseException(token.line, token.column, "'a' is only valid as a predicate");
  }
  if (token.kind == TokenKind::kError) {
    throw ParseException(token.line, token.column, token.text);
  }
  if (token.kind == TokenKind::kLBracket || token.kind == TokenKind::kLParen) {
    Unsupported(token, "blank node property lists and collections");
  }
  throw ParseException(token.line, token.column, "expected a term");
}

auto Parser::ParseFilter() -> std::unique_ptr<AstFilter> {
  Expect(TokenKind::kFilter, "FILTER");
  return ParseFilterAnd();
}

auto Parser::ParseFilterAnd() -> std::unique_ptr<AstFilter> {
  auto expr = ParseFilterPrimary();
  while (Match(TokenKind::kAndAnd)) {
    expr = AstFilter::And(std::move(expr), ParseFilterPrimary());
  }
  return expr;
}

auto Parser::ParseFilterPrimary() -> std::unique_ptr<AstFilter> {
  if (Match(TokenKind::kLParen)) {
    auto expr = ParseFilterAnd();
    Expect(TokenKind::kRParen, "')'");
    return expr;
  }
  AstTerm left = ParseTerm();
  AstFilter::Op op = AstFilter::Op::kEq;
  const Token op_token = Peek();
  if (Match(TokenKind::kEq)) {
    op = AstFilter::Op::kEq;
  } else if (Match(TokenKind::kNe)) {
    op = AstFilter::Op::kNe;
  } else if (Match(TokenKind::kLt)) {
    op = AstFilter::Op::kLt;
  } else if (Match(TokenKind::kGt)) {
    op = AstFilter::Op::kGt;
  } else if (Match(TokenKind::kLe)) {
    op = AstFilter::Op::kLe;
  } else if (Match(TokenKind::kGe)) {
    op = AstFilter::Op::kGe;
  } else {
    throw ParseException(op_token.line, op_token.column, "expected a comparison operator");
  }
  return AstFilter::Compare(op, std::move(left), ParseTerm());
}

void Parser::ParseSolutionModifier(AstQuery* query) {
  if (Match(TokenKind::kOrder)) {
    Expect(TokenKind::kBy, "BY");
    do {
      bool ascending = true;
      if (Match(TokenKind::kDesc)) {
        ascending = false;
        Expect(TokenKind::kLParen, "'('");
      } else if (Match(TokenKind::kAsc)) {
        Expect(TokenKind::kLParen, "'('");
      }
      if (Peek().kind != TokenKind::kVar) {
        throw ParseException(Peek().line, Peek().column, "ORDER BY needs a variable");
      }
      const Token var = Next();
      if (!ascending || Peek().kind == TokenKind::kRParen) {
        Expect(TokenKind::kRParen, "')'");
      }
      query->order_by.push_back(AstOrderKey{var.text, ascending, var.line, var.column});
    } while (Peek().kind == TokenKind::kVar || Peek().kind == TokenKind::kAsc ||
             Peek().kind == TokenKind::kDesc);
  }
  if (Peek().kind == TokenKind::kGroup) {
    Unsupported(Peek(), "GROUP BY");
  }
  if (Peek().kind == TokenKind::kHaving) {
    Unsupported(Peek(), "HAVING");
  }
  auto take_int = [&](std::optional<uint64_t>* dest) {
    if (Peek().kind != TokenKind::kInteger) {
      throw ParseException(Peek().line, Peek().column, "expected an integer");
    }
    *dest = ParseInteger();
  };
  bool saw_limit = false;
  bool saw_offset = false;
  while (Peek().kind == TokenKind::kLimit || Peek().kind == TokenKind::kOffset) {
    if (Match(TokenKind::kLimit)) {
      if (saw_limit) {
        throw ParseException(Peek().line, Peek().column, "LIMIT is repeated");
      }
      saw_limit = true;
      take_int(&query->limit);
    } else {
      Next();
      if (saw_offset) {
        throw ParseException(Peek().line, Peek().column, "OFFSET is repeated");
      }
      saw_offset = true;
      take_int(&query->offset);
    }
  }
}

auto Parser::ParseInteger() -> uint64_t {
  const Token token = Expect(TokenKind::kInteger, "an integer");
  uint64_t value = 0;
  for (char c : token.text) {
    value = value * 10 + static_cast<uint64_t>(c - '0');
  }
  return value;
}

}  // namespace ontodb
