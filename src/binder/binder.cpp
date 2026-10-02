#include "binder/binder.h"

#include "common/config.h"
#include "common/exception.h"
#include "common/term_codec.h"

namespace ontodb {
namespace {

auto ResolveDatatype(const AstTerm& term,
                     const std::unordered_map<std::string, std::string>& prefixes) -> std::string {
  if (term.extra.empty()) {
    return {};
  }
  if (term.extra[0] == '@') {
    return {};
  }
  if (!term.extra_is_prefixed) {
    const auto parsed = ParseCanonical(term.extra);
    return parsed.kind == ParsedTerm::Kind::kIri ? parsed.value : std::string{};
  }
  const auto colon = term.extra.find(':');
  const std::string prefix = term.extra.substr(0, colon);
  const std::string local = term.extra.substr(colon + 1);
  const auto it = prefixes.find(prefix);
  if (it == prefixes.end()) {
    return {};
  }
  return it->second + local;
}

}  // namespace

Binder::Binder(Dictionary& dictionary) : dictionary_(dictionary) {}

auto Binder::Intern(const std::string& canonical) -> term_id_t {
  return dictionary_.Insert(canonical);
}

auto Binder::Slot(const std::string& name, int line, int column) -> size_t {
  if (const auto it = slots_.find(name); it != slots_.end()) {
    return it->second;
  }
  if (line < 0) {
    throw BindException(1, 1, "variable ?" + name + " is not bound");
  }
  const size_t slot = var_names_.size();
  slots_.emplace(name, slot);
  var_names_.push_back(name);
  (void)line;
  (void)column;
  return slot;
}

auto Binder::BindTerm(const AstTerm& term, bool ground_only, bool intern) -> BoundTerm {
  BoundTerm bound;
  bound.text = term.Text();
  if (term.kind == AstTerm::Kind::kVariable) {
    if (ground_only) {
      throw BindException(term.line, term.column,
                          "INSERT DATA and DELETE DATA require ground terms");
    }
    bound.is_variable = true;
    bound.slot = Slot(term.value, term.line, term.column);
    bound.text = "?" + term.value;
    return bound;
  }

  std::string canonical;
  if (term.kind == AstTerm::Kind::kIri || term.kind == AstTerm::Kind::kRdfType) {
    canonical = CanonicalIri(term.value);
  } else if (term.kind == AstTerm::Kind::kBlank) {
    canonical = CanonicalBlank(term.value);
  } else if (term.kind == AstTerm::Kind::kPrefixed) {
    const auto it = prefixes_.find(term.value);
    if (it == prefixes_.end()) {
      throw BindException(term.line, term.column, "prefix '" + term.value + "' is not declared");
    }
    canonical = CanonicalIri(it->second + term.extra);
  } else if (term.kind == AstTerm::Kind::kLiteral) {
    std::string lang;
    std::string datatype;
    if (!term.extra.empty() && term.extra[0] == '@') {
      lang = term.extra.substr(1);
    } else if (!term.extra.empty()) {
      datatype = ResolveDatatype(term, prefixes_);
      if (datatype.empty() && term.extra_is_prefixed) {
        const auto colon = term.extra.find(':');
        throw BindException(term.line, term.column,
                            "prefix '" + term.extra.substr(0, colon) + "' is not declared");
      }
    }
    canonical = CanonicalLiteral(term.value, datatype, lang);
  } else {
    throw BindException(term.line, term.column, "unsupported term");
  }
  bound.text = canonical;
  if (intern) {
    bound.id = Intern(canonical);
  } else if (const auto existing = dictionary_.Lookup(canonical)) {
    bound.id = *existing;
  } else {
    bound.id = INVALID_TERM_ID;
  }
  return bound;
}

auto Binder::BindFilter(const AstFilter& filter) -> std::unique_ptr<BoundFilter> {
  auto bound = std::make_unique<BoundFilter>();
  bound->op = static_cast<BoundFilter::Op>(filter.op);
  if (filter.op == AstFilter::Op::kAnd) {
    bound->lhs = BindFilter(*filter.lhs);
    bound->rhs = BindFilter(*filter.rhs);
    bound->text = "(" + bound->lhs->text + " && " + bound->rhs->text + ")";
    return bound;
  }
  bound->left = BindTerm(filter.left, false, true);
  bound->right = BindTerm(filter.right, false, true);
  const char* symbol = "=";
  switch (filter.op) {
    case AstFilter::Op::kEq:
      symbol = "=";
      break;
    case AstFilter::Op::kNe:
      symbol = "!=";
      break;
    case AstFilter::Op::kLt:
      symbol = "<";
      break;
    case AstFilter::Op::kGt:
      symbol = ">";
      break;
    case AstFilter::Op::kLe:
      symbol = "<=";
      break;
    case AstFilter::Op::kGe:
      symbol = ">=";
      break;
    case AstFilter::Op::kAnd:
      break;
  }
  bound->text = bound->left.text + " " + symbol + " " + bound->right.text;
  return bound;
}

auto Binder::Bind(const AstQuery& query) -> BoundQuery {
  prefixes_.clear();
  slots_.clear();
  var_names_.clear();
  for (const auto& prefix : query.prefixes) {
    if (prefixes_.contains(prefix.first)) {
      throw BindException(1, 1, "prefix '" + prefix.first + "' is declared twice");
    }
    prefixes_.emplace(prefix.first, prefix.second);
  }

  BoundQuery bound;
  bound.kind = static_cast<BoundQuery::Kind>(query.kind);
  bound.distinct = query.distinct;
  bound.star = query.star;
  bound.limit = query.limit;
  bound.offset = query.offset;

  const bool ground = query.kind != AstQuery::Kind::kSelect;
  const bool intern = query.kind != AstQuery::Kind::kDelete;
  for (const auto& triple : query.triples) {
    BoundTriplePattern pattern;
    pattern.subject = BindTerm(triple.subject, ground, intern);
    pattern.predicate = BindTerm(triple.predicate, ground, intern);
    pattern.object = BindTerm(triple.object, ground, intern);
    pattern.text = pattern.subject.text + " " + pattern.predicate.text + " " + pattern.object.text;
    if (ground) {
      if (pattern.subject.id != INVALID_TERM_ID && pattern.predicate.id != INVALID_TERM_ID &&
          pattern.object.id != INVALID_TERM_ID) {
        bound.ground_triples.push_back(
            Triple{pattern.subject.id, pattern.predicate.id, pattern.object.id});
      }
    } else {
      bound.patterns.push_back(std::move(pattern));
    }
  }
  if (query.filter != nullptr) {
    if (ground) {
      throw BindException(1, 1, "FILTER is not valid in INSERT DATA or DELETE DATA");
    }
    bound.filter = BindFilter(*query.filter);
  }

  bound.var_names = var_names_;
  if (query.kind == AstQuery::Kind::kSelect) {
    if (query.star) {
      bound.projection_names = var_names_;
      for (size_t i = 0; i < var_names_.size(); i++) {
        bound.projection_slots.push_back(i);
      }
    } else {
      for (const auto& name : query.select_vars) {
        const auto it = slots_.find(name);
        if (it == slots_.end()) {
          throw BindException(1, 1, "variable ?" + name + " is not bound");
        }
        bound.projection_names.push_back(name);
        bound.projection_slots.push_back(it->second);
      }
    }
    for (const auto& key : query.order_by) {
      const auto it = slots_.find(key.variable);
      if (it == slots_.end()) {
        throw BindException(key.line, key.column, "variable ?" + key.variable + " is not bound");
      }
      bound.order_by.push_back(BoundOrderKey{it->second, key.ascending, key.variable});
    }
  }
  return bound;
}

}  // namespace ontodb
