#include "execution/executors/algebra_executor.h"

#include <map>
#include <set>
#include <utility>

#include "common/config.h"
#include "common/term_codec.h"
#include "concurrency/lock_manager.h"
#include "concurrency/lock_resources.h"
#include "concurrency/transaction.h"
#include "dictionary/dictionary.h"
#include "execution/term_compare.h"
#include "store/triple_store.h"

namespace ontodb {
namespace {

constexpr char kDefaultGraph[] = "urn:ontodb:graph";

auto Extend(Row row, const BoundTerm& term, term_id_t value) -> std::optional<Row> {
  if (!term.is_variable) {
    return value == term.id ? std::optional<Row>(std::move(row)) : std::nullopt;
  }
  if (row[term.slot] == INVALID_TERM_ID) {
    row[term.slot] = value;
    return row;
  }
  return row[term.slot] == value ? std::optional<Row>(std::move(row)) : std::nullopt;
}

auto FilterHolds(const BoundFilter& filter, const Row& row, const Dictionary& dictionary) -> bool {
  if (filter.op == BoundFilter::Op::kAnd) {
    return FilterHolds(*filter.lhs, row, dictionary) && FilterHolds(*filter.rhs, row, dictionary);
  }
  const term_id_t left = filter.left.is_variable ? row[filter.left.slot] : filter.left.id;
  const term_id_t right = filter.right.is_variable ? row[filter.right.slot] : filter.right.id;
  if (left == INVALID_TERM_ID || right == INVALID_TERM_ID) {
    return false;
  }
  return CompareTerms(left, right, filter.op, dictionary);
}

auto Compatible(const Row& left, const Row& right) -> bool {
  const size_t n = std::min(left.size(), right.size());
  for (size_t i = 0; i < n; i++) {
    if (left[i] != INVALID_TERM_ID && right[i] != INVALID_TERM_ID && left[i] != right[i]) {
      return false;
    }
  }
  return true;
}

auto Merge(Row left, const Row& right) -> Row {
  for (size_t i = 0; i < right.size() && i < left.size(); i++) {
    if (left[i] == INVALID_TERM_ID) {
      left[i] = right[i];
    }
  }
  return left;
}

class Machine {
 public:
  Machine(ExecutorContext* ctx, size_t width) : ctx_(ctx), width_(width) {}

  auto Run(const BoundPattern& pattern) -> std::vector<Row> {
    Row seed(width_, INVALID_TERM_ID);
    return Eval(pattern, {seed});
  }

 private:
  auto Eval(const BoundPattern& pattern, const std::vector<Row>& incoming) -> std::vector<Row> {
    switch (pattern.kind) {
      case BoundPattern::Kind::kJoin: {
        std::vector<Row> rows = incoming;
        for (const auto& child : pattern.children) {
          if (child->kind == BoundPattern::Kind::kOptional) {
            rows = LeftJoin(rows, *child->children.at(0));
          } else if (child->kind == BoundPattern::Kind::kMinus) {
            rows = Minus(rows, *child->children.at(0));
          } else {
            rows = Join(rows, *child);
          }
        }
        return rows;
      }
      case BoundPattern::Kind::kOptional:
        return LeftJoin(incoming, *pattern.children.at(0));
      case BoundPattern::Kind::kUnion: {
        std::vector<Row> rows;
        for (const auto& child : pattern.children) {
          auto part = Eval(*child, incoming);
          rows.insert(rows.end(), part.begin(), part.end());
        }
        return rows;
      }
      case BoundPattern::Kind::kMinus:
        return Minus(incoming, *pattern.children.at(0));
      case BoundPattern::Kind::kFilter: {
        std::vector<Row> rows;
        for (const auto& row : incoming) {
          if (FilterHolds(*pattern.filter, row, *ctx_->GetDictionary())) {
            rows.push_back(row);
          }
        }
        return rows;
      }
      case BoundPattern::Kind::kBind: {
        std::vector<Row> rows;
        for (auto row : incoming) {
          const term_id_t value =
              pattern.bind_expr.is_variable ? row[pattern.bind_expr.slot] : pattern.bind_expr.id;
          if (row[pattern.bind_slot] == INVALID_TERM_ID || row[pattern.bind_slot] == value) {
            row[pattern.bind_slot] = value;
            rows.push_back(std::move(row));
          }
        }
        return rows;
      }
      case BoundPattern::Kind::kValues:
        return JoinValues(incoming, pattern);
      case BoundPattern::Kind::kGraph:
        return EvalGraph(pattern, incoming);
      case BoundPattern::Kind::kService:
        try {
          return Eval(*pattern.children.at(0), incoming);
        } catch (const std::exception&) {
          if (pattern.silent) {
            return {};
          }
          throw;
        }
      case BoundPattern::Kind::kTriples:
        return JoinTriples(incoming, pattern);
    }
    return {};
  }

  auto Join(const std::vector<Row>& left, const BoundPattern& right) -> std::vector<Row> {
    std::vector<Row> rows;
    for (const auto& outer : left) {
      for (auto& inner : Eval(right, {outer})) {
        if (Compatible(outer, inner)) {
          rows.push_back(Merge(outer, inner));
        }
      }
    }
    return rows;
  }

  auto LeftJoin(const std::vector<Row>& left, const BoundPattern& right) -> std::vector<Row> {
    std::vector<Row> rows;
    for (const auto& outer : left) {
      auto matches = Eval(right, {outer});
      bool any = false;
      for (auto& inner : matches) {
        if (!Compatible(outer, inner)) {
          continue;
        }
        any = true;
        rows.push_back(Merge(outer, inner));
      }
      if (!any) {
        rows.push_back(outer);
      }
    }
    return rows;
  }

  auto Minus(const std::vector<Row>& left, const BoundPattern& right) -> std::vector<Row> {
    const auto others = Eval(right, {Row(width_, INVALID_TERM_ID)});
    std::vector<Row> rows;
    for (const auto& outer : left) {
      bool blocked = false;
      for (const auto& inner : others) {
        if (Compatible(outer, inner) && SharesABinding(outer, inner)) {
          blocked = true;
          break;
        }
      }
      if (!blocked) {
        rows.push_back(outer);
      }
    }
    return rows;
  }

  static auto SharesABinding(const Row& left, const Row& right) -> bool {
    const size_t n = std::min(left.size(), right.size());
    for (size_t i = 0; i < n; i++) {
      if (left[i] != INVALID_TERM_ID && right[i] != INVALID_TERM_ID) {
        return true;
      }
    }
    return false;
  }

  auto JoinValues(const std::vector<Row>& incoming,
                  const BoundPattern& pattern) -> std::vector<Row> {
    std::vector<Row> rows;
    for (const auto& outer : incoming) {
      for (const auto& table : pattern.values_rows) {
        Row row = outer;
        bool ok = true;
        for (size_t i = 0; i < pattern.values_slots.size() && i < table.size(); i++) {
          const size_t slot = pattern.values_slots[i];
          const term_id_t value = table[i];
          if (value == INVALID_TERM_ID) {
            continue;
          }
          if (row[slot] != INVALID_TERM_ID && row[slot] != value) {
            ok = false;
            break;
          }
          row[slot] = value;
        }
        if (ok) {
          rows.push_back(std::move(row));
        }
      }
    }
    return rows;
  }

  auto EvalGraph(const BoundPattern& pattern,
                 const std::vector<Row>& incoming) -> std::vector<Row> {
    const term_id_t graph = ctx_->GetDictionary()->Insert(CanonicalIri(kDefaultGraph));
    if (!pattern.graph_is_var && pattern.graph.id != INVALID_TERM_ID && pattern.graph.id != graph &&
        pattern.graph.text != CanonicalIri(kDefaultGraph) && !pattern.graph.text.empty()) {
      const auto id = ctx_->GetDictionary()->Lookup(pattern.graph.text);
      if (!id.has_value()) {
        return {};
      }
      if (*id != graph) {
        return {};
      }
    }
    std::vector<Row> seeded;
    for (auto row : incoming) {
      if (pattern.graph_is_var) {
        if (row[pattern.graph_slot] == INVALID_TERM_ID) {
          row[pattern.graph_slot] = graph;
        } else if (row[pattern.graph_slot] != graph) {
          continue;
        }
      }
      seeded.push_back(std::move(row));
    }
    return Eval(*pattern.children.at(0), seeded);
  }

  auto JoinTriples(const std::vector<Row>& incoming,
                   const BoundPattern& pattern) -> std::vector<Row> {
    std::vector<Row> rows = incoming;
    for (size_t i = 0; i < pattern.triples.size(); i++) {
      std::vector<Row> next;
      const bool path = i < pattern.paths.size() && pattern.paths[i] != nullptr;
      for (const auto& row : rows) {
        if (!path) {
          for (auto& match : ScanTriple(pattern.triples[i], row)) {
            next.push_back(std::move(match));
          }
        } else {
          for (auto& match : ScanPath(*pattern.paths[i], pattern.triples[i], row)) {
            next.push_back(std::move(match));
          }
        }
      }
      rows = std::move(next);
    }
    return rows;
  }

  auto ScanTriple(const BoundTriplePattern& pattern, const Row& row) -> std::vector<Row> {
    auto bound = [&](const BoundTerm& term) -> std::optional<term_id_t> {
      if (!term.is_variable) {
        return term.id == INVALID_TERM_ID ? std::optional<term_id_t>{}
                                          : std::optional<term_id_t>(term.id);
      }
      if (row[term.slot] != INVALID_TERM_ID) {
        return row[term.slot];
      }
      return std::nullopt;
    };
    if ((!pattern.subject.is_variable && pattern.subject.id == INVALID_TERM_ID) ||
        (!pattern.predicate.is_variable && pattern.predicate.id == INVALID_TERM_ID) ||
        (!pattern.object.is_variable && pattern.object.id == INVALID_TERM_ID)) {
      return {};
    }
    TriplePattern lookup;
    lookup.subject = bound(pattern.subject);
    lookup.predicate = bound(pattern.predicate);
    lookup.object = bound(pattern.object);
    auto iter = ctx_->GetStore()->Scan(lookup);
    iter->Init();
    std::vector<Row> rows;
    Triple triple;
    while (iter->Next(&triple)) {
      auto next = Extend(row, pattern.subject, triple.subject);
      if (!next.has_value()) {
        continue;
      }
      next = Extend(*next, pattern.predicate, triple.predicate);
      if (!next.has_value()) {
        continue;
      }
      next = Extend(*next, pattern.object, triple.object);
      if (next.has_value()) {
        rows.push_back(std::move(*next));
      }
    }
    return rows;
  }

  auto ScanPath(const BoundPath& path, const BoundTriplePattern& ends,
                const Row& row) -> std::vector<Row> {
    std::optional<term_id_t> subject;
    std::optional<term_id_t> object;
    if (!ends.subject.is_variable) {
      subject = ends.subject.id;
    } else if (row[ends.subject.slot] != INVALID_TERM_ID) {
      subject = row[ends.subject.slot];
    }
    if (!ends.object.is_variable) {
      object = ends.object.id;
    } else if (row[ends.object.slot] != INVALID_TERM_ID) {
      object = row[ends.object.slot];
    }
    std::vector<Row> rows;
    for (const auto& [from, to] : Walk(path, subject, object)) {
      auto next = Extend(row, ends.subject, from);
      if (!next.has_value()) {
        continue;
      }
      next = Extend(*next, ends.object, to);
      if (next.has_value()) {
        rows.push_back(std::move(*next));
      }
    }
    return rows;
  }

  auto Walk(const BoundPath& path, std::optional<term_id_t> from,
            std::optional<term_id_t> to) -> std::vector<std::pair<term_id_t, term_id_t>> {
    using Edge = std::pair<term_id_t, term_id_t>;
    if (path.op == BoundPath::Op::kLink) {
      TriplePattern lookup;
      lookup.predicate = path.link.id;
      lookup.subject = from;
      lookup.object = to;
      if (path.link.id == INVALID_TERM_ID) {
        return {};
      }
      auto iter = ctx_->GetStore()->Scan(lookup);
      iter->Init();
      std::vector<Edge> edges;
      Triple triple;
      while (iter->Next(&triple)) {
        edges.emplace_back(triple.subject, triple.object);
      }
      return edges;
    }
    if (path.op == BoundPath::Op::kInverse) {
      auto edges = Walk(*path.parts.at(0), to, from);
      for (auto& edge : edges) {
        std::swap(edge.first, edge.second);
      }
      return edges;
    }
    if (path.op == BoundPath::Op::kAlt) {
      std::vector<Edge> edges;
      for (const auto& part : path.parts) {
        auto more = Walk(*part, from, to);
        edges.insert(edges.end(), more.begin(), more.end());
      }
      return edges;
    }
    if (path.op == BoundPath::Op::kSeq) {
      std::vector<Edge> edges = Walk(*path.parts.at(0), from, std::nullopt);
      for (size_t i = 1; i < path.parts.size(); i++) {
        const bool last = i + 1 == path.parts.size();
        auto step = Walk(*path.parts[i], std::nullopt, last ? to : std::nullopt);
        std::map<term_id_t, std::vector<term_id_t>> next;
        for (const auto& edge : step) {
          next[edge.first].push_back(edge.second);
        }
        std::vector<Edge> joined;
        for (const auto& edge : edges) {
          const auto it = next.find(edge.second);
          if (it == next.end()) {
            continue;
          }
          for (const term_id_t dest : it->second) {
            joined.emplace_back(edge.first, dest);
          }
        }
        edges = std::move(joined);
      }
      return edges;
    }
    const bool plus = path.op == BoundPath::Op::kOneOrMore;
    auto hops = Walk(*path.parts.at(0), std::nullopt, std::nullopt);
    std::map<term_id_t, std::set<term_id_t>> adj;
    std::set<term_id_t> nodes;
    for (const auto& edge : hops) {
      adj[edge.first].insert(edge.second);
      nodes.insert(edge.first);
      nodes.insert(edge.second);
    }
    std::vector<term_id_t> starts;
    if (from.has_value()) {
      starts.push_back(*from);
    } else {
      starts.assign(nodes.begin(), nodes.end());
    }
    std::vector<Edge> edges;
    for (const term_id_t start : starts) {
      std::set<term_id_t> seen;
      std::vector<term_id_t> queue;
      if (!plus) {
        if (!to.has_value() || *to == start) {
          edges.emplace_back(start, start);
        }
        seen.insert(start);
      }
      queue.push_back(start);
      for (size_t q = 0; q < queue.size(); q++) {
        const auto it = adj.find(queue[q]);
        if (it == adj.end()) {
          continue;
        }
        for (const term_id_t dest : it->second) {
          if (!seen.insert(dest).second) {
            continue;
          }
          if (!to.has_value() || *to == dest) {
            edges.emplace_back(start, dest);
          }
          queue.push_back(dest);
        }
      }
    }
    return edges;
  }

  ExecutorContext* ctx_;
  size_t width_;
};

}  // namespace

AlgebraExecutor::AlgebraExecutor(ExecutorContext* ctx, const AlgebraPlan* plan)
    : ctx_(ctx), plan_(plan) {}

void AlgebraExecutor::Init() {
  ready_ = false;
  at_ = 0;
  rows_.clear();
}

auto AlgebraExecutor::Next(Row* row) -> bool {
  if (!ready_) {
    Transaction* txn = ctx_->GetTransaction();
    if (txn != nullptr && txn->GetIsolationLevel() != IsolationLevel::kReadUncommitted &&
        !txn->GetSnapshot().has_value()) {
      LockManager* locks = ctx_->GetLockManager();
      locks->Lock(txn, DatabaseResource(), LockMode::kIs, ctx_->GetSession());
      const bool range = txn->GetIsolationLevel() == IsolationLevel::kRepeatableRead;
      locks->Lock(txn, TriplesResource(), range ? LockMode::kS : LockMode::kIs, ctx_->GetSession());
    }
    rows_ = Machine(ctx_, plan_->Width()).Run(plan_->Pattern());
    ready_ = true;
  }
  if (at_ >= rows_.size()) {
    return false;
  }
  *row = rows_[at_++];
  return true;
}

auto AlgebraExecutor::GetOutputSchema() const -> const Schema& {
  return plan_->OutputSchema();
}

}  // namespace ontodb
