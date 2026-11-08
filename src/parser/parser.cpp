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
  } else if (Match(TokenKind::kReduced)) {
    query->reduced = true;
  }
  if (Match(TokenKind::kStar)) {
    query->star = true;
  } else {
    while (Peek().kind == TokenKind::kVar || Peek().kind == TokenKind::kLParen) {
      if (Match(TokenKind::kLParen)) {
        query->aggregates.push_back(ParseAggregate());
        query->projection_order.push_back(query->aggregates.back().alias);
      } else {
        query->select_vars.push_back(Next().text);
        query->projection_order.push_back(query->select_vars.back());
      }
      Match(TokenKind::kComma);
    }
    if (query->select_vars.empty() && query->aggregates.empty()) {
      throw ParseException(Peek().line, Peek().column, "expected select variables or *");
    }
  }
  Match(TokenKind::kWhere);
  InstallPattern(query, ParseBraced());
  ParseSolutionModifier(query);
}

void Parser::ParseUpdate(AstQuery* query, AstQuery::Kind kind) {
  query->kind = kind;
  if (Match(TokenKind::kData)) {
    Expect(TokenKind::kLBrace, "'{'");
    ParseGroup(query);
    Expect(TokenKind::kRBrace, "'}'");
    return;
  }
  query->update_where = true;
  if (kind == AstQuery::Kind::kDelete && Peek().kind == TokenKind::kWhere) {
    Next();
    auto where = ParseBraced();
    const auto harvest = [](const auto& self, const AstPattern* node,
                            std::vector<AstTriple>* out) -> void {
      if (node->kind == AstPattern::Kind::kTriples) {
        for (const auto& triple : node->triples) {
          if (triple.path != nullptr) {
            throw ParseException(triple.subject.line, triple.subject.column,
                                 "DELETE WHERE deletes triples, not paths");
          }
          AstTriple copy;
          copy.subject = triple.subject;
          copy.predicate = triple.predicate;
          copy.object = triple.object;
          out->push_back(std::move(copy));
        }
      }
      for (const auto& child : node->children) {
        self(self, child.get(), out);
      }
    };
    harvest(harvest, where.get(), &query->delete_template);
    InstallPattern(query, std::move(where));
    return;
  }
  auto take_template = [&](std::vector<AstTriple>* dest) {
    Expect(TokenKind::kLBrace, "'{'");
    auto body = ParseGroupBody();
    Expect(TokenKind::kRBrace, "'}'");
    if (body->kind == AstPattern::Kind::kTriples) {
      *dest = std::move(body->triples);
    } else if (body->kind == AstPattern::Kind::kJoin) {
      for (auto& child : body->children) {
        if (child->kind != AstPattern::Kind::kTriples) {
          throw ParseException(child->line, child->column, "a template holds triples, not a group");
        }
        dest->insert(dest->end(), std::make_move_iterator(child->triples.begin()),
                     std::make_move_iterator(child->triples.end()));
      }
    }
  };
  take_template(kind == AstQuery::Kind::kDelete ? &query->delete_template
                                                : &query->insert_template);
  if (kind == AstQuery::Kind::kDelete && Match(TokenKind::kInsert)) {
    take_template(&query->insert_template);
  }
  Expect(TokenKind::kWhere, "WHERE");
  InstallPattern(query, ParseBraced());
}

void Parser::ParseGroup(AstQuery* query) {
  auto body = ParseGroupBody();
  if (body->kind == AstPattern::Kind::kTriples) {
    for (auto& triple : body->triples) {
      query->triples.push_back(std::move(triple));
    }
    return;
  }
  if (body->kind == AstPattern::Kind::kJoin) {
    for (auto& child : body->children) {
      if (child->kind == AstPattern::Kind::kTriples) {
        for (auto& triple : child->triples) {
          query->triples.push_back(std::move(triple));
        }
      } else if (child->kind == AstPattern::Kind::kFilter) {
        if (query->filter == nullptr) {
          query->filter = std::move(child->filter);
        } else {
          query->filter = AstFilter::And(std::move(query->filter), std::move(child->filter));
        }
      } else {
        throw ParseException(child->line, child->column,
                             "INSERT DATA and DELETE DATA hold triples");
      }
    }
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
  if (token.kind == TokenKind::kLBracket) {
    return ParseBlankPropertyList();
  }
  if (token.kind == TokenKind::kLParen) {
    return ParseCollection();
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
  if (Match(TokenKind::kGroup)) {
    Expect(TokenKind::kBy, "BY");
    do {
      if (Peek().kind != TokenKind::kVar) {
        throw ParseException(Peek().line, Peek().column, "GROUP BY needs a variable");
      }
      query->group_by.push_back(Next().text);
    } while (Peek().kind == TokenKind::kVar);
  }
  if (Match(TokenKind::kHaving)) {
    query->having = ParseFilterAnd();
  }
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

namespace {

auto IsGroupStart(TokenKind kind) -> bool {
  return kind == TokenKind::kFilter || kind == TokenKind::kOptional || kind == TokenKind::kMinus ||
         kind == TokenKind::kBind || kind == TokenKind::kValues || kind == TokenKind::kGraph ||
         kind == TokenKind::kService || kind == TokenKind::kLBrace;
}

auto ClonePath(const AstPath& path) -> std::unique_ptr<AstPath> {
  auto copy = std::make_unique<AstPath>();
  copy->op = path.op;
  copy->link = path.link;
  for (const auto& part : path.parts) {
    copy->parts.push_back(ClonePath(*part));
  }
  return copy;
}

}  // namespace

void Parser::InstallPattern(AstQuery* query, std::unique_ptr<AstPattern> pattern) {
  bool flat = true;
  auto consider = [&](const AstPattern* node) {
    if (node->kind == AstPattern::Kind::kTriples) {
      for (const auto& triple : node->triples) {
        if (triple.path != nullptr) {
          flat = false;
        }
      }
      return;
    }
    if (node->kind != AstPattern::Kind::kFilter) {
      flat = false;
    }
  };
  if (pattern->kind == AstPattern::Kind::kJoin) {
    for (const auto& child : pattern->children) {
      consider(child.get());
    }
  } else {
    consider(pattern.get());
  }
  if (!flat) {
    query->pattern = std::move(pattern);
    return;
  }
  auto take = [&](AstPattern* node) {
    if (node->kind == AstPattern::Kind::kTriples) {
      for (auto& triple : node->triples) {
        query->triples.push_back(std::move(triple));
      }
    } else if (node->kind == AstPattern::Kind::kFilter) {
      if (query->filter == nullptr) {
        query->filter = std::move(node->filter);
      } else {
        query->filter = AstFilter::And(std::move(query->filter), std::move(node->filter));
      }
    }
  };
  if (pattern->kind == AstPattern::Kind::kJoin) {
    for (auto& child : pattern->children) {
      take(child.get());
    }
  } else {
    take(pattern.get());
  }
}

auto Parser::ParseBraced() -> std::unique_ptr<AstPattern> {
  Expect(TokenKind::kLBrace, "'{'");
  auto left = ParseGroupBody();
  Expect(TokenKind::kRBrace, "'}'");
  if (Peek().kind != TokenKind::kUnion) {
    return left;
  }
  auto uni = std::make_unique<AstPattern>();
  uni->kind = AstPattern::Kind::kUnion;
  uni->line = left->line;
  uni->column = left->column;
  uni->children.push_back(std::move(left));
  while (Match(TokenKind::kUnion)) {
    Expect(TokenKind::kLBrace, "'{'");
    uni->children.push_back(ParseGroupBody());
    Expect(TokenKind::kRBrace, "'}'");
  }
  return uni;
}

auto Parser::ParseGroupBody() -> std::unique_ptr<AstPattern> {
  auto join = std::make_unique<AstPattern>();
  join->kind = AstPattern::Kind::kJoin;
  join->line = Peek().line;
  join->column = Peek().column;
  while (Peek().kind != TokenKind::kRBrace && Peek().kind != TokenKind::kEof) {
    const Token start = Peek();
    std::unique_ptr<AstPattern> element;
    if (Peek().kind == TokenKind::kFilter) {
      element = std::make_unique<AstPattern>();
      element->kind = AstPattern::Kind::kFilter;
      element->filter = ParseFilter();
      element->line = start.line;
      element->column = start.column;
    } else if (Match(TokenKind::kOptional)) {
      element = std::make_unique<AstPattern>();
      element->kind = AstPattern::Kind::kOptional;
      element->line = start.line;
      element->column = start.column;
      element->children.push_back(ParseBraced());
    } else if (Match(TokenKind::kMinus)) {
      element = std::make_unique<AstPattern>();
      element->kind = AstPattern::Kind::kMinus;
      element->line = start.line;
      element->column = start.column;
      element->children.push_back(ParseBraced());
    } else if (Match(TokenKind::kBind)) {
      element = std::make_unique<AstPattern>();
      element->kind = AstPattern::Kind::kBind;
      element->line = start.line;
      element->column = start.column;
      Expect(TokenKind::kLParen, "'('");
      element->bind_expr = ParseTerm();
      Expect(TokenKind::kAs, "AS");
      if (Peek().kind != TokenKind::kVar) {
        throw ParseException(Peek().line, Peek().column, "BIND needs a variable");
      }
      element->bind_var = Next().text;
      Expect(TokenKind::kRParen, "')'");
    } else if (Peek().kind == TokenKind::kValues) {
      element = ParseValues();
    } else if (Match(TokenKind::kGraph)) {
      element = std::make_unique<AstPattern>();
      element->kind = AstPattern::Kind::kGraph;
      element->line = start.line;
      element->column = start.column;
      if (Peek().kind == TokenKind::kVar) {
        element->graph_is_var = true;
        element->graph = ParseTerm();
      } else {
        element->graph = ParseTerm();
      }
      element->children.push_back(ParseBraced());
    } else if (Match(TokenKind::kService)) {
      element = std::make_unique<AstPattern>();
      element->kind = AstPattern::Kind::kService;
      element->line = start.line;
      element->column = start.column;
      element->silent = Match(TokenKind::kSilent);
      element->graph = ParseTerm();
      element->children.push_back(ParseBraced());
    } else if (Peek().kind == TokenKind::kLBrace) {
      element = ParseBraced();
    } else {
      element = ParseTriplesBlock();
    }
    join->children.push_back(std::move(element));
    Match(TokenKind::kDot);
    if (IsGroupStart(Peek().kind)) {
      continue;
    }
  }
  if (join->children.size() == 1) {
    return std::move(join->children[0]);
  }
  return join;
}

auto Parser::ParseTriplesBlock() -> std::unique_ptr<AstPattern> {
  auto block = std::make_unique<AstPattern>();
  block->kind = AstPattern::Kind::kTriples;
  block->line = Peek().line;
  block->column = Peek().column;
  ParseTriplesSameSubject(&block->triples);
  while (Match(TokenKind::kDot)) {
    if (Peek().kind == TokenKind::kRBrace || Peek().kind == TokenKind::kEof ||
        IsGroupStart(Peek().kind)) {
      break;
    }
    ParseTriplesSameSubject(&block->triples);
  }
  return block;
}

void Parser::ParseTriplesSameSubject(std::vector<AstTriple>* out) {
  const auto saved = sink_;
  sink_ = out;
  const AstTerm subject = ParseTerm();
  bool any = false;
  while (true) {
    auto path = ParsePath();
    const std::vector<AstTerm> objects = ParseObjectList();
    const bool link = path->op == AstPath::Op::kLink && path->parts.empty();
    for (size_t i = 0; i < objects.size(); i++) {
      AstTriple triple;
      triple.subject = subject;
      triple.object = objects[i];
      if (link) {
        triple.predicate = path->link;
      } else if (i + 1 == objects.size()) {
        triple.path = std::move(path);
      } else {
        triple.path = ClonePath(*path);
      }
      out->push_back(std::move(triple));
    }
    any = true;
    if (!Match(TokenKind::kSemicolon)) {
      break;
    }
    if (Peek().kind == TokenKind::kDot || Peek().kind == TokenKind::kRBrace ||
        Peek().kind == TokenKind::kEof || IsGroupStart(Peek().kind)) {
      break;
    }
  }
  sink_ = saved;
  if (!any) {
    throw ParseException(Peek().line, Peek().column, "expected a predicate");
  }
}

auto Parser::ParsePath() -> std::unique_ptr<AstPath> {
  auto left = ParsePathSeq();
  if (Peek().kind != TokenKind::kPipe) {
    return left;
  }
  auto alt = std::make_unique<AstPath>();
  alt->op = AstPath::Op::kAlt;
  alt->parts.push_back(std::move(left));
  while (Match(TokenKind::kPipe)) {
    alt->parts.push_back(ParsePathSeq());
  }
  return alt;
}

auto Parser::ParsePathSeq() -> std::unique_ptr<AstPath> {
  auto left = ParsePathElt();
  if (Peek().kind != TokenKind::kSlash) {
    return left;
  }
  auto seq = std::make_unique<AstPath>();
  seq->op = AstPath::Op::kSeq;
  seq->parts.push_back(std::move(left));
  while (Match(TokenKind::kSlash)) {
    seq->parts.push_back(ParsePathElt());
  }
  return seq;
}

auto Parser::ParsePathElt() -> std::unique_ptr<AstPath> {
  const bool inverse = Match(TokenKind::kCaret);
  std::unique_ptr<AstPath> path;
  if (Match(TokenKind::kLParen)) {
    path = ParsePath();
    Expect(TokenKind::kRParen, "')'");
  } else {
    path = std::make_unique<AstPath>();
    path->op = AstPath::Op::kLink;
    path->link = ParseVerb();
  }
  if (inverse) {
    auto inv = std::make_unique<AstPath>();
    inv->op = AstPath::Op::kInverse;
    inv->parts.push_back(std::move(path));
    path = std::move(inv);
  }
  std::optional<AstPath::Op> mod;
  if (Match(TokenKind::kStar)) {
    mod = AstPath::Op::kZeroOrMore;
  } else if (Match(TokenKind::kPlus)) {
    mod = AstPath::Op::kOneOrMore;
  } else if (Match(TokenKind::kQuestion)) {
    mod = AstPath::Op::kZeroOrOne;
  }
  if (!mod.has_value()) {
    return path;
  }
  auto wrapped = std::make_unique<AstPath>();
  wrapped->op = *mod;
  wrapped->parts.push_back(std::move(path));
  return wrapped;
}

auto Parser::FreshBlank() -> AstTerm {
  AstTerm term;
  term.kind = AstTerm::Kind::kBlank;
  term.value = "b" + std::to_string(++blanks_);
  term.line = Peek().line;
  term.column = Peek().column;
  return term;
}

auto Parser::ParseBlankPropertyList() -> AstTerm {
  const Token open = Expect(TokenKind::kLBracket, "'['");
  AstTerm blank = FreshBlank();
  blank.line = open.line;
  blank.column = open.column;
  if (Match(TokenKind::kRBracket)) {
    return blank;
  }
  if (sink_ == nullptr) {
    throw ParseException(open.line, open.column, "a blank node property list needs a triple");
  }
  const AstTerm subject = blank;
  while (true) {
    auto path = ParsePath();
    for (const auto& object : ParseObjectList()) {
      AstTriple triple;
      triple.subject = subject;
      triple.object = object;
      if (path->op == AstPath::Op::kLink && path->parts.empty()) {
        triple.predicate = path->link;
      } else {
        triple.path = ClonePath(*path);
      }
      sink_->push_back(std::move(triple));
    }
    if (!Match(TokenKind::kSemicolon)) {
      break;
    }
    if (Peek().kind == TokenKind::kRBracket) {
      break;
    }
  }
  Expect(TokenKind::kRBracket, "']'");
  return blank;
}

auto Parser::ParseCollection() -> AstTerm {
  const Token open = Expect(TokenKind::kLParen, "'('");
  if (sink_ == nullptr) {
    throw ParseException(open.line, open.column, "a collection needs a triple");
  }
  std::vector<AstTerm> items;
  while (Peek().kind != TokenKind::kRParen && Peek().kind != TokenKind::kEof) {
    items.push_back(ParseTerm());
  }
  Expect(TokenKind::kRParen, "')'");
  AstTerm nil;
  nil.kind = AstTerm::Kind::kIri;
  nil.value = "http://www.w3.org/1999/02/22-rdf-syntax-ns#nil";
  nil.line = open.line;
  nil.column = open.column;
  AstTerm head = nil;
  for (auto it = items.rbegin(); it != items.rend(); ++it) {
    AstTerm node = FreshBlank();
    AstTerm first;
    first.kind = AstTerm::Kind::kIri;
    first.value = "http://www.w3.org/1999/02/22-rdf-syntax-ns#first";
    AstTerm rest;
    rest.kind = AstTerm::Kind::kIri;
    rest.value = "http://www.w3.org/1999/02/22-rdf-syntax-ns#rest";
    sink_->push_back(AstTriple{node, first, *it, nullptr});
    sink_->push_back(AstTriple{node, rest, head, nullptr});
    head = node;
  }
  return head;
}

auto Parser::ParseAggregate() -> AstAggregate {
  AstAggregate agg;
  agg.line = Peek().line;
  agg.column = Peek().column;
  const Token which = Peek();
  if (Match(TokenKind::kCount)) {
    agg.op = AstAggregate::Op::kCount;
  } else if (Match(TokenKind::kSum)) {
    agg.op = AstAggregate::Op::kSum;
  } else if (Match(TokenKind::kAvg)) {
    agg.op = AstAggregate::Op::kAvg;
  } else if (Match(TokenKind::kMin)) {
    agg.op = AstAggregate::Op::kMin;
  } else if (Match(TokenKind::kMax)) {
    agg.op = AstAggregate::Op::kMax;
  } else if (Match(TokenKind::kSample)) {
    agg.op = AstAggregate::Op::kSample;
  } else {
    throw ParseException(which.line, which.column, "expected an aggregate");
  }
  Expect(TokenKind::kLParen, "'('");
  agg.distinct = Match(TokenKind::kDistinct);
  if (Match(TokenKind::kStar)) {
    agg.star = true;
  } else {
    agg.arg = ParseTerm();
  }
  Expect(TokenKind::kRParen, "')'");
  Expect(TokenKind::kAs, "AS");
  if (Peek().kind != TokenKind::kVar) {
    throw ParseException(Peek().line, Peek().column, "an aggregate needs a name");
  }
  agg.alias = Next().text;
  Expect(TokenKind::kRParen, "')'");
  return agg;
}

auto Parser::ParseValues() -> std::unique_ptr<AstPattern> {
  auto values = std::make_unique<AstPattern>();
  values->kind = AstPattern::Kind::kValues;
  const Token start = Expect(TokenKind::kValues, "VALUES");
  values->line = start.line;
  values->column = start.column;
  const bool row = Match(TokenKind::kLParen);
  if (Peek().kind != TokenKind::kVar) {
    throw ParseException(Peek().line, Peek().column, "VALUES needs a variable");
  }
  do {
    values->values_vars.push_back(Next().text);
  } while (Peek().kind == TokenKind::kVar);
  if (row) {
    Expect(TokenKind::kRParen, "')'");
  }
  Expect(TokenKind::kLBrace, "'{'");
  auto one = [&]() -> std::optional<AstTerm> {
    if (Match(TokenKind::kUndef)) {
      return std::nullopt;
    }
    return ParseTerm();
  };
  while (Peek().kind != TokenKind::kRBrace && Peek().kind != TokenKind::kEof) {
    std::vector<std::optional<AstTerm>> tuple;
    if (!row) {
      tuple.push_back(one());
    } else {
      Expect(TokenKind::kLParen, "'('");
      for (size_t i = 0; i < values->values_vars.size(); i++) {
        tuple.push_back(one());
      }
      Expect(TokenKind::kRParen, "')'");
    }
    values->values_rows.push_back(std::move(tuple));
  }
  Expect(TokenKind::kRBrace, "'}'");
  return values;
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
