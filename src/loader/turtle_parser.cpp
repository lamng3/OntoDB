#include "loader/turtle_parser.h"

#include <cctype>
#include <cstdint>
#include <map>
#include <stdexcept>

#include "common/term_codec.h"

namespace ontodb {
namespace {

constexpr const char* kRdf = "http://www.w3.org/1999/02/22-rdf-syntax-ns#";
constexpr const char* kXsd = "http://www.w3.org/2001/XMLSchema#";

class Parser {
 public:
  Parser(const std::string& text, const TurtleSink& sink) : s_(text), sink_(sink) {}

  void Run() {
    while (true) {
      Skip();
      if (pos_ >= s_.size()) {
        return;
      }
      Statement();
    }
  }

 private:
  [[noreturn]] void Fail(const std::string& what) const {
    int line = 1;
    for (size_t i = 0; i < pos_ && i < s_.size(); i++) {
      line += s_[i] == '\n';
    }
    throw std::runtime_error("Turtle error on line " + std::to_string(line) + ": " + what);
  }

  auto Peek() const -> char { return pos_ < s_.size() ? s_[pos_] : '\0'; }
  auto StartsWith(const char* word) const -> bool { return s_.compare(pos_, std::char_traits<char>::length(word), word) == 0; }

  void Skip() {
    while (pos_ < s_.size()) {
      const char c = s_[pos_];
      if (c == '#') {
        while (pos_ < s_.size() && s_[pos_] != '\n') {
          pos_++;
        }
      } else if (std::isspace(static_cast<unsigned char>(c))) {
        pos_++;
      } else {
        return;
      }
    }
  }

  void Expect(char c) {
    Skip();
    if (Peek() != c) {
      Fail(std::string("expected '") + c + "'");
    }
    pos_++;
  }

  auto KeywordAhead(const char* word) const -> bool {
    const size_t n = std::char_traits<char>::length(word);
    if (s_.size() < pos_ + n) {
      return false;
    }
    for (size_t i = 0; i < n; i++) {
      if (std::tolower(static_cast<unsigned char>(s_[pos_ + i])) != word[i]) {
        return false;
      }
    }
    return pos_ + n < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_ + n]));
  }

  void Statement() {
    if (Peek() == '@') {
      if (StartsWith("@prefix")) {
        pos_ += 7;
        Prefix();
        Expect('.');
      } else if (StartsWith("@base")) {
        pos_ += 5;
        Skip();
        base_ = ReadIri();
        Expect('.');
      } else {
        Fail("unknown directive");
      }
      return;
    }
    if (KeywordAhead("prefix")) {
      pos_ += 6;
      Prefix();
      return;
    }
    if (KeywordAhead("base")) {
      pos_ += 4;
      Skip();
      base_ = ReadIri();
      return;
    }
    const std::string subject = Subject();
    PredicateObjects(subject);
    Expect('.');
  }

  void Prefix() {
    Skip();
    std::string name;
    while (pos_ < s_.size() && s_[pos_] != ':') {
      name.push_back(s_[pos_++]);
    }
    Expect(':');
    Skip();
    prefixes_[name] = ReadIri();
  }

  auto ReadIri() -> std::string {
    if (Peek() != '<') {
      Fail("expected an IRI");
    }
    pos_++;
    std::string out;
    while (pos_ < s_.size() && s_[pos_] != '>') {
      if (s_[pos_] == '\\') {
        out += Escape(true);
      } else {
        out.push_back(s_[pos_++]);
      }
    }
    if (Peek() != '>') {
      Fail("unterminated IRI");
    }
    pos_++;
    return Resolve(out);
  }

  auto Resolve(const std::string& iri) const -> std::string {
    if (iri.find(':') != std::string::npos || base_.empty()) {
      return iri;
    }
    if (iri.empty()) {
      return base_;
    }
    if (iri[0] == '#') {
      const size_t hash = base_.find('#');
      return base_.substr(0, hash) + iri;
    }
    if (iri[0] == '/') {
      const size_t scheme = base_.find("://");
      const size_t slash = scheme == std::string::npos ? std::string::npos : base_.find('/', scheme + 3);
      return base_.substr(0, slash) + iri;
    }
    const size_t slash = base_.rfind('/');
    return (slash == std::string::npos ? base_ : base_.substr(0, slash + 1)) + iri;
  }

  static void AppendUtf8(std::string* out, uint32_t cp) {
    if (cp < 0x80) {
      out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  // At a backslash: consumes the escape and returns its text.
  auto Escape(bool iri) -> std::string {
    pos_++;
    const char c = Peek();
    pos_++;
    if (c == 'u' || c == 'U') {
      const int digits = c == 'u' ? 4 : 8;
      if (s_.size() < pos_ + digits) {
        Fail("short \\u escape");
      }
      uint32_t cp = 0;
      for (int i = 0; i < digits; i++) {
        const char h = s_[pos_++];
        if (!std::isxdigit(static_cast<unsigned char>(h))) {
          Fail("bad \\u escape");
        }
        cp = cp * 16 + static_cast<uint32_t>(std::isdigit(static_cast<unsigned char>(h)) ? h - '0' : std::tolower(h) - 'a' + 10);
      }
      std::string out;
      AppendUtf8(&out, cp);
      return out;
    }
    if (iri) {
      Fail("bad escape in IRI");
    }
    switch (c) {
      case 't': return "\t";
      case 'n': return "\n";
      case 'r': return "\r";
      case 'b': return "\b";
      case 'f': return "\f";
      case '"': return "\"";
      case '\'': return "'";
      case '\\': return "\\";
      default: Fail("bad escape");
    }
  }

  static auto NameChar(char c) -> bool {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || (static_cast<unsigned char>(c) >= 0x80);
  }

  auto PrefixedName() -> std::string {
    std::string prefix;
    while (pos_ < s_.size() && s_[pos_] != ':' && NameChar(s_[pos_])) {
      prefix.push_back(s_[pos_++]);
    }
    if (Peek() != ':') {
      Fail("expected a prefixed name");
    }
    pos_++;
    std::string local;
    while (pos_ < s_.size()) {
      const char c = s_[pos_];
      if (NameChar(c) || c == ':' || c == '%') {
        local.push_back(c);
        pos_++;
      } else if (c == '\\' && pos_ + 1 < s_.size()) {
        local.push_back(s_[pos_ + 1]);
        pos_ += 2;
      } else if (c == '.' && pos_ + 1 < s_.size() && (NameChar(s_[pos_ + 1]) || s_[pos_ + 1] == ':')) {
        local.push_back(c);
        pos_++;
      } else {
        break;
      }
    }
    const auto it = prefixes_.find(prefix);
    if (it == prefixes_.end()) {
      Fail("unknown prefix '" + prefix + ":'");
    }
    return it->second + local;
  }

  auto Blank() -> std::string { return CanonicalBlank("b" + std::to_string(++blanks_)); }

  void Emit(const std::string& s, const std::string& p, const std::string& o) { sink_(s, p, o); }

  auto Subject() -> std::string {
    Skip();
    const char c = Peek();
    if (c == '<') {
      return CanonicalIri(ReadIri());
    }
    if (c == '[') {
      return BlankPropertyList();
    }
    if (c == '(') {
      return Collection();
    }
    if (c == '_' && pos_ + 1 < s_.size() && s_[pos_ + 1] == ':') {
      return LabeledBlank();
    }
    return CanonicalIri(PrefixedName());
  }

  auto LabeledBlank() -> std::string {
    pos_ += 2;
    std::string id;
    while (pos_ < s_.size() && (NameChar(s_[pos_]) || (s_[pos_] == '.' && pos_ + 1 < s_.size() && NameChar(s_[pos_ + 1])))) {
      id.push_back(s_[pos_++]);
    }
    return CanonicalBlank("n" + id);
  }

  auto BlankPropertyList() -> std::string {
    pos_++;
    const std::string node = Blank();
    Skip();
    if (Peek() != ']') {
      PredicateObjects(node);
    }
    Expect(']');
    return node;
  }

  auto Collection() -> std::string {
    pos_++;
    const std::string nil = CanonicalIri(std::string(kRdf) + "nil");
    std::string head = nil;
    std::string last;
    while (true) {
      Skip();
      if (Peek() == ')') {
        pos_++;
        break;
      }
      const std::string cell = Blank();
      if (last.empty()) {
        head = cell;
      } else {
        Emit(last, CanonicalIri(std::string(kRdf) + "rest"), cell);
      }
      Emit(cell, CanonicalIri(std::string(kRdf) + "first"), Object());
      last = cell;
    }
    if (!last.empty()) {
      Emit(last, CanonicalIri(std::string(kRdf) + "rest"), nil);
    }
    return head;
  }

  void PredicateObjects(const std::string& subject) {
    while (true) {
      Skip();
      std::string predicate;
      if (Peek() == 'a' && pos_ + 1 < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_ + 1]))) {
        pos_++;
        predicate = CanonicalIri(std::string(kRdf) + "type");
      } else if (Peek() == '<') {
        predicate = CanonicalIri(ReadIri());
      } else {
        predicate = CanonicalIri(PrefixedName());
      }
      while (true) {
        Emit(subject, predicate, Object());
        Skip();
        if (Peek() != ',') {
          break;
        }
        pos_++;
      }
      Skip();
      if (Peek() != ';') {
        return;
      }
      while (Peek() == ';') {
        pos_++;
        Skip();
      }
      if (Peek() == '.' || Peek() == ']') {
        return;
      }
    }
  }

  auto Object() -> std::string {
    Skip();
    const char c = Peek();
    if (c == '<') {
      return CanonicalIri(ReadIri());
    }
    if (c == '[') {
      return BlankPropertyList();
    }
    if (c == '(') {
      return Collection();
    }
    if (c == '"' || c == '\'') {
      return Literal();
    }
    if (c == '_' && pos_ + 1 < s_.size() && s_[pos_ + 1] == ':') {
      return LabeledBlank();
    }
    if (std::isdigit(static_cast<unsigned char>(c)) || c == '+' || c == '-' || (c == '.' && pos_ + 1 < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_ + 1])))) {
      return Number();
    }
    if (StartsWith("true") && !NameChar(pos_ + 4 < s_.size() ? s_[pos_ + 4] : ' ') && (pos_ + 4 >= s_.size() || s_[pos_ + 4] != ':')) {
      pos_ += 4;
      return CanonicalLiteral("true", std::string(kXsd) + "boolean", "");
    }
    if (StartsWith("false") && !NameChar(pos_ + 5 < s_.size() ? s_[pos_ + 5] : ' ') && (pos_ + 5 >= s_.size() || s_[pos_ + 5] != ':')) {
      pos_ += 5;
      return CanonicalLiteral("false", std::string(kXsd) + "boolean", "");
    }
    return CanonicalIri(PrefixedName());
  }

  auto Number() -> std::string {
    std::string text;
    bool dot = false;
    bool exp = false;
    if (Peek() == '+' || Peek() == '-') {
      text.push_back(s_[pos_++]);
    }
    while (pos_ < s_.size()) {
      const char c = s_[pos_];
      if (std::isdigit(static_cast<unsigned char>(c))) {
        text.push_back(c);
        pos_++;
      } else if (c == '.' && !dot && !exp && pos_ + 1 < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_ + 1]))) {
        dot = true;
        text.push_back(c);
        pos_++;
      } else if ((c == 'e' || c == 'E') && !exp) {
        exp = true;
        text.push_back(c);
        pos_++;
        if (Peek() == '+' || Peek() == '-') {
          text.push_back(s_[pos_++]);
        }
      } else {
        break;
      }
    }
    const char* type = exp ? "double" : dot ? "decimal" : "integer";
    return CanonicalLiteral(text, std::string(kXsd) + type, "");
  }

  auto Literal() -> std::string {
    const char quote = s_[pos_];
    const bool long_form = s_.compare(pos_, 3, std::string(3, quote)) == 0;
    pos_ += long_form ? 3 : 1;
    std::string lex;
    while (true) {
      if (pos_ >= s_.size()) {
        Fail("unterminated string");
      }
      const char c = s_[pos_];
      if (c == quote) {
        if (!long_form) {
          pos_++;
          break;
        }
        if (s_.compare(pos_, 3, std::string(3, quote)) == 0) {
          pos_ += 3;
          break;
        }
        lex.push_back(c);
        pos_++;
      } else if (c == '\\') {
        lex += Escape(false);
      } else if (c == '\n' && !long_form) {
        Fail("newline in string");
      } else {
        lex.push_back(c);
        pos_++;
      }
    }
    if (Peek() == '@') {
      pos_++;
      std::string lang;
      while (pos_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '-')) {
        lang.push_back(s_[pos_++]);
      }
      return CanonicalLiteral(lex, "", lang);
    }
    if (StartsWith("^^")) {
      pos_ += 2;
      const std::string datatype = Peek() == '<' ? ReadIri() : PrefixedName();
      return CanonicalLiteral(lex, datatype, "");
    }
    return CanonicalLiteral(lex, "", "");
  }

  const std::string& s_;
  const TurtleSink& sink_;
  size_t pos_{0};
  std::string base_;
  std::map<std::string, std::string> prefixes_;
  uint64_t blanks_{0};
};

}  // namespace

void ParseTurtle(const std::string& text, const TurtleSink& sink) { Parser(text, sink).Run(); }

}  // namespace ontodb
