#include "common/term_codec.h"

#include "common/config.h"

namespace ontodb {
namespace {

auto HexValue(char c) -> int {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

void AppendUtf8(std::string* out, uint32_t cp) {
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

}  // namespace

auto EscapeLiteral(std::string_view lex) -> std::string {
  std::string out;
  out.reserve(lex.size());
  for (unsigned char c : lex) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(static_cast<char>(c));
        break;
    }
  }
  return out;
}

auto UnescapeLiteral(std::string_view lex) -> std::string {
  std::string out;
  out.reserve(lex.size());
  for (size_t i = 0; i < lex.size(); i++) {
    if (lex[i] != '\\' || i + 1 >= lex.size()) {
      out.push_back(lex[i]);
      continue;
    }
    const char n = lex[++i];
    switch (n) {
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      case '\\':
      case '"':
      case '\'':
        out.push_back(n);
        break;
      case 'u':
      case 'U': {
        const size_t digits = n == 'u' ? 4 : 8;
        uint32_t cp = 0;
        bool ok = i + digits < lex.size();
        for (size_t d = 0; ok && d < digits; d++) {
          const int h = HexValue(lex[i + 1 + d]);
          if (h < 0) {
            ok = false;
          } else {
            cp = (cp << 4) | static_cast<uint32_t>(h);
          }
        }
        if (!ok) {
          out.push_back('\\');
          out.push_back(n);
        } else {
          AppendUtf8(&out, cp);
          i += digits;
        }
        break;
      }
      default:
        out.push_back(n);
        break;
    }
  }
  return out;
}

auto CanonicalIri(std::string_view iri) -> std::string {
  std::string out;
  out.reserve(iri.size() + 2);
  out.push_back('<');
  out.append(iri);
  out.push_back('>');
  return out;
}

auto CanonicalLiteral(std::string_view lex, std::string_view datatype_iri,
                      std::string_view lang) -> std::string {
  std::string out;
  out.push_back('"');
  out += EscapeLiteral(lex);
  out.push_back('"');
  if (!lang.empty()) {
    out.push_back('@');
    out.append(lang);
  } else if (!datatype_iri.empty()) {
    out += "^^";
    out += CanonicalIri(datatype_iri);
  }
  return out;
}

auto CanonicalBlank(std::string_view id) -> std::string {
  std::string out = "_:";
  out.append(id);
  return out;
}

auto ParseCanonical(std::string_view text) -> ParsedTerm {
  ParsedTerm term;
  if (text.size() >= 2 && text.front() == '<' && text.back() == '>') {
    term.kind = ParsedTerm::Kind::kIri;
    term.value = std::string(text.substr(1, text.size() - 2));
    return term;
  }
  if (text.size() >= 2 && text[0] == '_' && text[1] == ':') {
    term.kind = ParsedTerm::Kind::kBlank;
    term.value = std::string(text.substr(2));
    return term;
  }
  if (text.empty() || text.front() != '"') {
    return term;
  }
  size_t i = 1;
  std::string raw;
  while (i < text.size()) {
    if (text[i] == '\\' && i + 1 < text.size()) {
      raw.push_back(text[i]);
      raw.push_back(text[i + 1]);
      i += 2;
      continue;
    }
    if (text[i] == '"') {
      break;
    }
    raw.push_back(text[i]);
    i++;
  }
  if (i >= text.size() || text[i] != '"') {
    return term;
  }
  term.kind = ParsedTerm::Kind::kLiteral;
  term.value = UnescapeLiteral(raw);
  i++;
  if (i < text.size() && text[i] == '@') {
    term.lang = std::string(text.substr(i + 1));
  } else if (i + 1 < text.size() && text[i] == '^' && text[i + 1] == '^') {
    const auto dt = text.substr(i + 2);
    if (dt.size() >= 2 && dt.front() == '<' && dt.back() == '>') {
      term.datatype = std::string(dt.substr(1, dt.size() - 2));
    }
  }
  return term;
}

auto IsNumericDatatype(std::string_view datatype_iri) -> bool {
  return datatype_iri == kXsdInteger || datatype_iri == kXsdDecimal || datatype_iri == kXsdDouble ||
         datatype_iri == kXsdFloat;
}

}  // namespace ontodb
