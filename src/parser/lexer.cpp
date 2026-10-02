#include "parser/lexer.h"

#include <cctype>

namespace ontodb {
namespace {

auto IsNameStart(char c) -> bool {
  return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

auto IsNameCont(char c) -> bool {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.';
}

auto Lower(std::string text) -> std::string {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return text;
}

auto Keyword(const std::string& ident) -> TokenKind {
  const std::string word = Lower(ident);
  if (word == "prefix")
    return TokenKind::kPrefix;
  if (word == "select")
    return TokenKind::kSelect;
  if (word == "distinct")
    return TokenKind::kDistinct;
  if (word == "where")
    return TokenKind::kWhere;
  if (word == "filter")
    return TokenKind::kFilter;
  if (word == "insert")
    return TokenKind::kInsert;
  if (word == "delete")
    return TokenKind::kDelete;
  if (word == "data")
    return TokenKind::kData;
  if (word == "order")
    return TokenKind::kOrder;
  if (word == "by")
    return TokenKind::kBy;
  if (word == "limit")
    return TokenKind::kLimit;
  if (word == "offset")
    return TokenKind::kOffset;
  if (word == "asc")
    return TokenKind::kAsc;
  if (word == "desc")
    return TokenKind::kDesc;
  if (word == "optional")
    return TokenKind::kOptional;
  if (word == "union")
    return TokenKind::kUnion;
  if (word == "minus")
    return TokenKind::kMinus;
  if (word == "bind")
    return TokenKind::kBind;
  if (word == "values")
    return TokenKind::kValues;
  if (word == "graph")
    return TokenKind::kGraph;
  if (word == "service")
    return TokenKind::kService;
  if (word == "construct")
    return TokenKind::kConstruct;
  if (word == "ask")
    return TokenKind::kAsk;
  if (word == "describe")
    return TokenKind::kDescribe;
  if (word == "group")
    return TokenKind::kGroup;
  if (word == "having")
    return TokenKind::kHaving;
  if (word == "reduced")
    return TokenKind::kReduced;
  if (word == "count")
    return TokenKind::kCount;
  if (word == "sum")
    return TokenKind::kSum;
  if (word == "avg")
    return TokenKind::kAvg;
  if (word == "min")
    return TokenKind::kMin;
  if (word == "max")
    return TokenKind::kMax;
  if (word == "sample")
    return TokenKind::kSample;
  if (word == "base")
    return TokenKind::kBase;
  if (ident == "a")
    return TokenKind::kA;
  return TokenKind::kIdent;
}

}  // namespace

Lexer::Lexer(std::string_view input) : input_(input) {}

auto Lexer::Peek() -> const Token& {
  if (!has_peek_) {
    peeked_ = Scan();
    has_peek_ = true;
  }
  return peeked_;
}

auto Lexer::Next() -> Token {
  Token token = Peek();
  has_peek_ = false;
  return token;
}

auto Lexer::StartsIri(size_t at) const -> bool {
  for (size_t i = at; i < input_.size(); i++) {
    const char c = input_[i];
    if (c == '>') {
      return true;
    }
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '<' || c == '"' || c == '{' ||
        c == '}') {
      return false;
    }
  }
  return false;
}

auto Lexer::Scan() -> Token {
  while (pos_ < input_.size()) {
    const char c = input_[pos_];
    if (c == ' ' || c == '\t' || c == '\r') {
      pos_++;
      column_++;
      continue;
    }
    if (c == '\n') {
      pos_++;
      line_++;
      column_ = 1;
      continue;
    }
    if (c == '#') {
      while (pos_ < input_.size() && input_[pos_] != '\n') {
        pos_++;
        column_++;
      }
      continue;
    }
    break;
  }

  Token token;
  token.line = line_;
  token.column = column_;
  if (pos_ >= input_.size()) {
    token.kind = TokenKind::kEof;
    return token;
  }

  const char c = input_[pos_];
  auto take = [&](TokenKind kind, std::string text, size_t n) {
    token.kind = kind;
    token.text = std::move(text);
    pos_ += n;
    column_ += static_cast<int>(n);
  };

  if (c == '{') {
    take(TokenKind::kLBrace, "{", 1);
    return token;
  }
  if (c == '}') {
    take(TokenKind::kRBrace, "}", 1);
    return token;
  }
  if (c == '(') {
    take(TokenKind::kLParen, "(", 1);
    return token;
  }
  if (c == ')') {
    take(TokenKind::kRParen, ")", 1);
    return token;
  }
  if (c == '[') {
    take(TokenKind::kLBracket, "[", 1);
    return token;
  }
  if (c == ';') {
    take(TokenKind::kSemicolon, ";", 1);
    return token;
  }
  if (c == ',') {
    take(TokenKind::kComma, ",", 1);
    return token;
  }
  if (c == '*') {
    take(TokenKind::kStar, "*", 1);
    return token;
  }
  if (c == '+') {
    take(TokenKind::kPlus, "+", 1);
    return token;
  }
  if (c == '|') {
    take(TokenKind::kPipe, "|", 1);
    return token;
  }
  if (c == '/') {
    take(TokenKind::kSlash, "/", 1);
    return token;
  }
  if (c == '.') {
    take(TokenKind::kDot, ".", 1);
    return token;
  }
  if (c == '&' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '&') {
    take(TokenKind::kAndAnd, "&&", 2);
    return token;
  }
  if (c == '!' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '=') {
    take(TokenKind::kNe, "!=", 2);
    return token;
  }
  if (c == '<' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '=') {
    take(TokenKind::kLe, "<=", 2);
    return token;
  }
  if (c == '>' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '=') {
    take(TokenKind::kGe, ">=", 2);
    return token;
  }
  if (c == '^' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '^') {
    take(TokenKind::kHatHat, "^^", 2);
    return token;
  }
  if (c == '=') {
    take(TokenKind::kEq, "=", 1);
    return token;
  }
  if (c == '>') {
    take(TokenKind::kGt, ">", 1);
    return token;
  }
  if (c == '<') {
    if (StartsIri(pos_ + 1)) {
      const size_t begin = pos_ + 1;
      size_t end = begin;
      while (end < input_.size() && input_[end] != '>') {
        end++;
      }
      take(TokenKind::kIri, std::string(input_.substr(begin, end - begin)), end - pos_ + 1);
      return token;
    }
    take(TokenKind::kLt, "<", 1);
    return token;
  }
  if (c == '?' || c == '$') {
    size_t end = pos_ + 1;
    if (end < input_.size() &&
        (IsNameStart(input_[end]) || std::isdigit(static_cast<unsigned char>(input_[end])))) {
      end++;
      while (end < input_.size() && IsNameCont(input_[end])) {
        end++;
      }
    }
    if (end == pos_ + 1) {
      token.kind = TokenKind::kError;
      token.text = "variable name expected";
      pos_ = end;
      column_ += 1;
      return token;
    }
    take(TokenKind::kVar, std::string(input_.substr(pos_ + 1, end - pos_ - 1)), end - pos_);
    return token;
  }
  if (c == '@') {
    size_t end = pos_ + 1;
    while (end < input_.size() &&
           (std::isalpha(static_cast<unsigned char>(input_[end])) || input_[end] == '-')) {
      end++;
    }
    take(TokenKind::kLang, std::string(input_.substr(pos_ + 1, end - pos_ - 1)), end - pos_);
    return token;
  }
  if (c == '"' || c == '\'') {
    const char quote = c;
    const bool long_string =
        pos_ + 2 < input_.size() && input_[pos_ + 1] == quote && input_[pos_ + 2] == quote;
    size_t i = pos_ + (long_string ? 3 : 1);
    std::string body;
    int line = line_;
    int col = column_ + (long_string ? 3 : 1);
    bool closed = false;
    while (i < input_.size()) {
      if (input_[i] == '\\' && i + 1 < input_.size()) {
        body.push_back('\\');
        body.push_back(input_[i + 1]);
        if (input_[i + 1] == '\n') {
          line++;
          col = 1;
        } else {
          col += 2;
        }
        i += 2;
        continue;
      }
      if (long_string && i + 2 < input_.size() && input_[i] == quote && input_[i + 1] == quote &&
          input_[i + 2] == quote) {
        i += 3;
        col += 3;
        closed = true;
        break;
      }
      if (!long_string && input_[i] == quote) {
        i++;
        col++;
        closed = true;
        break;
      }
      if (input_[i] == '\n') {
        line++;
        col = 1;
      } else {
        col++;
      }
      body.push_back(input_[i]);
      i++;
    }
    token.kind = closed ? TokenKind::kString : TokenKind::kError;
    token.text = closed ? body : "unterminated string";
    pos_ = i;
    line_ = line;
    column_ = col;
    return token;
  }
  if (c == '_' && pos_ + 1 < input_.size() && input_[pos_ + 1] == ':') {
    size_t end = pos_ + 2;
    while (end < input_.size() && IsNameCont(input_[end])) {
      end++;
    }
    take(TokenKind::kBlank, std::string(input_.substr(pos_ + 2, end - pos_ - 2)), end - pos_);
    return token;
  }
  if (std::isdigit(static_cast<unsigned char>(c))) {
    size_t end = pos_ + 1;
    while (end < input_.size() && std::isdigit(static_cast<unsigned char>(input_[end]))) {
      end++;
    }
    if (end < input_.size() && input_[end] == '.' && end + 1 < input_.size() &&
        std::isdigit(static_cast<unsigned char>(input_[end + 1]))) {
      end++;
      while (end < input_.size() && std::isdigit(static_cast<unsigned char>(input_[end]))) {
        end++;
      }
      take(TokenKind::kDecimal, std::string(input_.substr(pos_, end - pos_)), end - pos_);
      return token;
    }
    take(TokenKind::kInteger, std::string(input_.substr(pos_, end - pos_)), end - pos_);
    return token;
  }
  if (c == ':' || IsNameStart(c) ||
      (c == 'h' && pos_ + 7 < input_.size() && input_.substr(pos_, 7) == "http://") ||
      (c == 'h' && pos_ + 8 < input_.size() && input_.substr(pos_, 8) == "https://")) {
    if (input_.substr(pos_, 7) == "http://" || input_.substr(pos_, 8) == "https://") {
      size_t end = pos_;
      while (end < input_.size()) {
        const char ch = input_[end];
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '{' || ch == '}' ||
            ch == ';' || ch == ',' || ch == ')' || ch == '>') {
          break;
        }
        // A dot ends a triple. A dot inside a hostname or path stays in the IRI.
        if (ch == '.' &&
            (end + 1 >= input_.size() || input_[end + 1] == ' ' || input_[end + 1] == '\t' ||
             input_[end + 1] == '\n' || input_[end + 1] == '\r' || input_[end + 1] == '}')) {
          break;
        }
        end++;
      }
      take(TokenKind::kIri, std::string(input_.substr(pos_, end - pos_)), end - pos_);
      return token;
    }
    size_t end = pos_;
    if (c != ':') {
      end++;
      while (end < input_.size() && IsNameCont(input_[end]) && input_[end] != '.') {
        end++;
      }
    }
    if (end < input_.size() && input_[end] == ':') {
      const std::string prefix(input_.substr(pos_, end - pos_));
      size_t local = end + 1;
      while (local < input_.size() && IsNameCont(input_[local])) {
        local++;
      }
      token.kind = TokenKind::kPrefixed;
      token.text = prefix + "\n" + std::string(input_.substr(end + 1, local - end - 1));
      const size_t n = local - pos_;
      pos_ = local;
      column_ += static_cast<int>(n);
      return token;
    }
    const std::string ident(input_.substr(pos_, end - pos_));
    take(Keyword(ident), ident, end - pos_);
    return token;
  }

  token.kind = TokenKind::kError;
  token.text = std::string("unexpected character '") + c + "'";
  pos_++;
  column_++;
  return token;
}

auto TokenName(TokenKind kind) -> const char* {
  switch (kind) {
    case TokenKind::kEof:
      return "end of query";
    case TokenKind::kPrefix:
      return "PREFIX";
    case TokenKind::kSelect:
      return "SELECT";
    case TokenKind::kVar:
      return "variable";
    case TokenKind::kIri:
      return "IRI";
    case TokenKind::kPrefixed:
      return "prefixed name";
    case TokenKind::kString:
      return "string";
    default:
      return "token";
  }
}

}  // namespace ontodb
