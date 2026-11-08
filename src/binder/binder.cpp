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

auto Binder::BindPath(const AstPath& path) -> std::unique_ptr<BoundPath> {
  auto bound = std::make_unique<BoundPath>();
  bound->op = static_cast<BoundPath::Op>(path.op);
  if (path.op == AstPath::Op::kLink) {
    bound->link = BindTerm(path.link, false, false);
  }
  for (const auto& part : path.parts) {
    bound->parts.push_back(BindPath(*part));
  }
  return bound;
}

auto Binder::BindTriple(const AstTriple& triple, bool ground_only,
                        bool intern) -> BoundTriplePattern {
  BoundTriplePattern pattern;
  pattern.subject = BindTerm(triple.subject, ground_only, intern);
  pattern.object = BindTerm(triple.object, ground_only, intern);
  if (triple.path != nullptr) {
    pattern.predicate.text = "path";
  } else {
    pattern.predicate = BindTerm(triple.predicate, ground_only, intern);
  }
  pattern.text = pattern.subject.text + " " + pattern.predicate.text + " " + pattern.object.text;
  return pattern;
}

auto Binder::BindPattern(const AstPattern& pattern) -> std::unique_ptr<BoundPattern> {
  auto bound = std::make_unique<BoundPattern>();
  bound->kind = static_cast<BoundPattern::Kind>(pattern.kind);
  bound->label = "pattern";
  for (const auto& child : pattern.children) {
    bound->children.push_back(BindPattern(*child));
  }
  for (const auto& triple : pattern.triples) {
    bound->triples.push_back(BindTriple(triple, false, false));
    if (triple.path != nullptr) {
      bound->paths.push_back(BindPath(*triple.path));
    } else {
      bound->paths.push_back(nullptr);
    }
  }
  if (pattern.filter != nullptr) {
    bound->filter = BindFilter(*pattern.filter);
  }
  if (pattern.kind == AstPattern::Kind::kBind) {
    bound->bind_expr = BindTerm(pattern.bind_expr, false, true);
    bound->bind_slot = Slot(pattern.bind_var, pattern.line, pattern.column);
  }
  if (pattern.kind == AstPattern::Kind::kValues) {
    for (const auto& name : pattern.values_vars) {
      bound->values_slots.push_back(Slot(name, pattern.line, pattern.column));
    }
    for (const auto& row : pattern.values_rows) {
      std::vector<term_id_t> ids;
      for (const auto& cell : row) {
        if (!cell.has_value()) {
          ids.push_back(INVALID_TERM_ID);
        } else {
          ids.push_back(BindTerm(*cell, false, true).id);
        }
      }
      bound->values_rows.push_back(std::move(ids));
    }
  }
  if (pattern.kind == AstPattern::Kind::kGraph || pattern.kind == AstPattern::Kind::kService) {
    bound->graph_is_var = pattern.graph_is_var;
    bound->silent = pattern.silent;
    if (pattern.graph_is_var) {
      bound->graph_slot = Slot(pattern.graph.value, pattern.graph.line, pattern.graph.column);
    } else if (pattern.graph.kind != AstTerm::Kind::kVariable) {
      bound->graph = BindTerm(pattern.graph, false, false);
    }
  }
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

  const bool ground = query.kind != AstQuery::Kind::kSelect && !query.update_where;
  // Only INSERT DATA adds terms to the dictionary. A query or a delete looks
  // them up: a term the store has never seen matches nothing, so the pattern
  // can be answered (and locked) as empty without writing the dictionary.
  // A WHERE template may introduce a term on the insert side, so those are interned.
  const bool intern = query.kind == AstQuery::Kind::kInsert && !query.update_where;
  if (query.pattern != nullptr) {
    bound.pattern = std::shared_ptr<BoundPattern>(BindPattern(*query.pattern).release());
  }
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
  bound.update_where = query.update_where;
  for (const auto& triple : query.delete_template) {
    bound.delete_template.push_back(BindTriple(triple, false, false));
  }
  for (const auto& triple : query.insert_template) {
    bound.insert_template.push_back(BindTriple(triple, false, true));
  }
  for (const auto& aggregate : query.aggregates) {
    BoundAggregate bound_aggregate;
    bound_aggregate.op = static_cast<BoundAggregate::Op>(aggregate.op);
    bound_aggregate.distinct = aggregate.distinct;
    bound_aggregate.star = aggregate.star;
    bound_aggregate.alias = aggregate.alias;
    if (!aggregate.star) {
      const BoundTerm arg = BindTerm(aggregate.arg, false, false);
      if (!arg.is_variable) {
        throw BindException(aggregate.line, aggregate.column,
                            "an aggregate argument is a variable or *");
      }
      bound_aggregate.arg = arg.slot;
    }
    bound.aggregates.push_back(bound_aggregate);
  }
  for (const auto& name : query.group_by) {
    const auto it = slots_.find(name);
    if (it == slots_.end()) {
      throw BindException(1, 1, "variable ?" + name + " is not bound");
    }
    bound.group_slots.push_back(it->second);
  }

  bound.var_names = var_names_;
  const bool grouped = !bound.aggregates.empty() || !bound.group_slots.empty();
  std::unordered_map<std::string, size_t> output_slot;
  if (grouped) {
    size_t index = 0;
    for (const size_t slot : bound.group_slots) {
      output_slot.emplace(var_names_[slot], index++);
    }
    for (const auto& aggregate : bound.aggregates) {
      output_slot.emplace(aggregate.alias, index++);
    }
  }
  if (query.kind == AstQuery::Kind::kSelect) {
    if (query.star && !grouped) {
      bound.projection_names = var_names_;
      for (size_t i = 0; i < var_names_.size(); i++) {
        bound.projection_slots.push_back(i);
      }
    } else if (grouped) {
      const auto& names =
          query.projection_order.empty() ? query.select_vars : query.projection_order;
      for (const auto& name : names) {
        const auto it = output_slot.find(name);
        if (it == output_slot.end()) {
          throw BindException(1, 1, "variable ?" + name + " is not grouped or aggregated");
        }
        bound.projection_names.push_back(name);
        bound.projection_slots.push_back(it->second);
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
      size_t slot = 0;
      if (grouped) {
        const auto it = output_slot.find(key.variable);
        if (it == output_slot.end()) {
          throw BindException(key.line, key.column, "variable ?" + key.variable + " is not bound");
        }
        slot = it->second;
      } else {
        const auto it = slots_.find(key.variable);
        if (it == slots_.end()) {
          throw BindException(key.line, key.column, "variable ?" + key.variable + " is not bound");
        }
        slot = it->second;
      }
      bound.order_by.push_back(BoundOrderKey{slot, key.ascending, key.variable});
    }
    if (query.having != nullptr) {
      if (!grouped) {
        throw BindException(1, 1, "HAVING needs GROUP BY or an aggregate");
      }
      const auto saved_slots = slots_;
      const auto saved_names = var_names_;
      slots_.clear();
      for (const auto& [name, slot] : output_slot) {
        slots_.emplace(name, slot);
      }
      bound.having = BindFilter(*query.having);
      slots_ = saved_slots;
      var_names_ = saved_names;
    }
  }
  return bound;
}

}  // namespace ontodb
