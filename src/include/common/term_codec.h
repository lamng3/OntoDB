#pragma once

#include <string>
#include <string_view>

namespace ontodb {

// Canonical spellings shared by the loader and the binder so the same RDF
// term always interned to the same id:
//   IRI      <http://example/a>
//   literal  "lex"            "lex"@en     "lex"^^<datatype>
//   blank    _:id
auto EscapeLiteral(std::string_view lex) -> std::string;
auto UnescapeLiteral(std::string_view lex) -> std::string;
auto CanonicalIri(std::string_view iri) -> std::string;
auto CanonicalLiteral(std::string_view lex, std::string_view datatype_iri,
                      std::string_view lang) -> std::string;
auto CanonicalBlank(std::string_view id) -> std::string;

struct ParsedTerm {
  enum class Kind { kIri, kLiteral, kBlank, kInvalid };
  Kind kind{Kind::kInvalid};
  std::string value;
  std::string datatype;
  std::string lang;
};

auto ParseCanonical(std::string_view text) -> ParsedTerm;
auto IsNumericDatatype(std::string_view datatype_iri) -> bool;

}  // namespace ontodb
