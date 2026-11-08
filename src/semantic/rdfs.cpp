#include "semantic/rdfs.h"

#include "common/config.h"
#include "common/term_codec.h"

namespace ontodb {
namespace {

constexpr char kSubClassOf[] = "http://www.w3.org/2000/01/rdf-schema#subClassOf";
constexpr char kSubPropertyOf[] = "http://www.w3.org/2000/01/rdf-schema#subPropertyOf";
constexpr char kDomain[] = "http://www.w3.org/2000/01/rdf-schema#domain";
constexpr char kRange[] = "http://www.w3.org/2000/01/rdf-schema#range";

auto Find(Dictionary& dictionary, const char* iri) -> std::optional<term_id_t> {
  return dictionary.Lookup(CanonicalIri(iri));
}

}  // namespace

auto RdfsClosure(const std::set<Triple>& triples, Dictionary& dictionary) -> std::set<Triple> {
  const auto sub = Find(dictionary, kSubClassOf);
  const auto sub_property = Find(dictionary, kSubPropertyOf);
  const auto domain = Find(dictionary, kDomain);
  const auto range = Find(dictionary, kRange);
  const auto type = Find(dictionary, kRdfType);
  std::set<Triple> have = triples;
  bool grew = true;
  while (grew) {
    grew = false;
    const std::set<Triple> seen = have;
    auto add = [&](const Triple& triple) {
      if (have.insert(triple).second) {
        grew = true;
      }
    };
    for (const auto& first : seen) {
      for (const auto& second : seen) {
        if (sub.has_value() && first.predicate == *sub && second.predicate == *sub &&
            first.object == second.subject) {
          add(Triple{first.subject, *sub, second.object});
        }
        if (sub_property.has_value() && first.predicate == *sub_property &&
            second.predicate == *sub_property && first.object == second.subject) {
          add(Triple{first.subject, *sub_property, second.object});
        }
        if (type.has_value() && sub.has_value() && first.predicate == *type &&
            second.predicate == *sub && first.object == second.subject) {
          add(Triple{first.subject, *type, second.object});
        }
        if (sub_property.has_value() && second.predicate == *sub_property &&
            first.predicate == second.subject) {
          add(Triple{first.subject, second.object, first.object});
        }
      }
      if (domain.has_value() && type.has_value()) {
        for (const auto& rule : seen) {
          if (rule.predicate == *domain && rule.subject == first.predicate) {
            add(Triple{first.subject, *type, rule.object});
          }
        }
      }
      if (range.has_value() && type.has_value()) {
        for (const auto& rule : seen) {
          if (rule.predicate == *range && rule.subject == first.predicate) {
            add(Triple{first.object, *type, rule.object});
          }
        }
      }
    }
  }
  return have;
}

}  // namespace ontodb
