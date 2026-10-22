#include "semantic/class_hierarchy.h"

#include <algorithm>
#include <cstddef>

namespace ontodb {

namespace {

const std::vector<Interval> kNoIntervals;
const std::vector<term_id_t> kNoClasses;

// Strongly connected components of the graph "class to parent", iteratively.
// Returns the component of each node and the component count. Components are
// numbered in the order Tarjan completes them, so a component is numbered
// after every component it points to: parents before children.
auto Components(const std::vector<std::vector<uint32_t>>& up, std::vector<uint32_t>* comp) -> uint32_t {
  const size_t n = up.size();
  constexpr uint32_t kNone = static_cast<uint32_t>(-1);
  std::vector<uint32_t> index(n, kNone), low(n, 0), next_edge(n, 0);
  std::vector<bool> on_stack(n, false);
  std::vector<uint32_t> stack, call;
  comp->assign(n, kNone);
  uint32_t counter = 0, count = 0;
  for (uint32_t root = 0; root < n; root++) {
    if (index[root] != kNone) {
      continue;
    }
    call.push_back(root);
    while (!call.empty()) {
      const uint32_t v = call.back();
      if (next_edge[v] == 0 && index[v] == kNone) {
        index[v] = low[v] = counter++;
        stack.push_back(v);
        on_stack[v] = true;
      }
      if (next_edge[v] < up[v].size()) {
        const uint32_t w = up[v][next_edge[v]++];
        if (index[w] == kNone) {
          call.push_back(w);
        } else if (on_stack[w]) {
          low[v] = std::min(low[v], index[w]);
        }
        continue;
      }
      if (low[v] == index[v]) {
        uint32_t w;
        do {
          w = stack.back();
          stack.pop_back();
          on_stack[w] = false;
          (*comp)[w] = count;
        } while (w != v);
        count++;
      }
      call.pop_back();
      if (!call.empty()) {
        low[call.back()] = std::min(low[call.back()], low[v]);
      }
    }
  }
  return count;
}

void Coalesce(std::vector<Interval>* set) {
  std::sort(set->begin(), set->end(), [](const Interval& a, const Interval& b) { return a.lo < b.lo; });
  size_t out = 0;
  for (const Interval& next : *set) {
    if (out > 0 && next.lo <= (*set)[out - 1].hi + 1) {
      (*set)[out - 1].hi = std::max((*set)[out - 1].hi, next.hi);
    } else {
      (*set)[out++] = next;
    }
  }
  set->resize(out);
}

}  // namespace

auto ClassHierarchy::Build(const std::vector<std::pair<term_id_t, term_id_t>>& edges, TreeParent rule)
    -> ClassHierarchy {
  ClassHierarchy h;
  std::vector<term_id_t> ids;
  std::unordered_map<term_id_t, uint32_t> node_of;
  auto node = [&](term_id_t id) {
    auto [it, fresh] = node_of.emplace(id, static_cast<uint32_t>(ids.size()));
    if (fresh) {
      ids.push_back(id);
    }
    return it->second;
  };
  std::vector<std::pair<uint32_t, uint32_t>> arcs;
  for (const auto& [child, parent] : edges) {
    arcs.emplace_back(node(child), node(parent));
  }
  const size_t n = ids.size();
  std::vector<std::vector<uint32_t>> up(n);
  for (const auto& [c, p] : arcs) {
    if (c != p) {
      up[c].push_back(p);
    }
  }
  std::vector<uint32_t> comp;
  const uint32_t m = Components(up, &comp);
  h.stats_.classes = n;
  h.stats_.components = m;
  h.members_.assign(m, {});
  for (uint32_t v = 0; v < n; v++) {
    h.members_[comp[v]].push_back(ids[v]);
    h.comp_of_.emplace(ids[v], comp[v]);
  }
  // The condensed DAG, with distinct edges. Tarjan numbered parents first, so
  // processing components in increasing order visits every parent before its
  // children.
  h.parents_.assign(m, {});
  std::vector<std::vector<uint32_t>> children(m);
  for (uint32_t v = 0; v < n; v++) {
    for (const uint32_t w : up[v]) {
      if (comp[v] != comp[w]) {
        h.parents_[comp[v]].push_back(comp[w]);
      }
    }
  }
  for (uint32_t c = 0; c < m; c++) {
    std::vector<uint32_t>& ps = h.parents_[c];
    std::sort(ps.begin(), ps.end());
    ps.erase(std::unique(ps.begin(), ps.end()), ps.end());
  }
  // kFirst needs the order the edges were listed in, not the sorted order.
  std::vector<uint32_t> first_parent(m, static_cast<uint32_t>(-1));
  for (const auto& [c, p] : arcs) {
    if (comp[c] != comp[p] && first_parent[comp[c]] == static_cast<uint32_t>(-1)) {
      first_parent[comp[c]] = comp[p];
    }
  }
  std::vector<uint32_t> depth(m, 0), tree_parent(m, static_cast<uint32_t>(-1));
  for (uint32_t c = 0; c < m; c++) {
    for (const uint32_t p : h.parents_[c]) {
      children[p].push_back(c);
      h.stats_.edges++;
      depth[c] = std::max(depth[c], depth[p] + 1);
    }
    if (h.parents_[c].empty()) {
      continue;
    }
    if (rule == TreeParent::kFirst) {
      tree_parent[c] = first_parent[c];
    } else {
      uint32_t best = h.parents_[c][0];
      for (const uint32_t p : h.parents_[c]) {
        if (depth[p] > depth[best] ||
            (depth[p] == depth[best] && h.members_[p][0] < h.members_[best][0])) {
          best = p;
        }
      }
      tree_parent[c] = best;
    }
  }
  // Postorder over the spanning forest. Roots and children are visited in
  // the order of their smallest term id, so the labels do not depend on how
  // the edges were listed.
  std::vector<std::vector<uint32_t>> tree_children(m);
  std::vector<uint32_t> roots;
  for (uint32_t c = 0; c < m; c++) {
    if (tree_parent[c] == static_cast<uint32_t>(-1)) {
      roots.push_back(c);
    } else {
      tree_children[tree_parent[c]].push_back(c);
    }
  }
  const auto by_id = [&](uint32_t a, uint32_t b) {
    return *std::min_element(h.members_[a].begin(), h.members_[a].end()) <
           *std::min_element(h.members_[b].begin(), h.members_[b].end());
  };
  std::sort(roots.begin(), roots.end(), by_id);
  for (auto& list : tree_children) {
    std::sort(list.begin(), list.end(), by_id);
  }
  h.label_.assign(m, 0);
  h.by_label_.reserve(m);
  std::vector<uint32_t> lo(m, 0);
  uint32_t counter = 0;
  std::vector<std::pair<uint32_t, size_t>> walk;
  for (const uint32_t root : roots) {
    walk.emplace_back(root, 0);
    lo[root] = counter + 1;
    while (!walk.empty()) {
      auto& [v, i] = walk.back();
      if (i < tree_children[v].size()) {
        const uint32_t w = tree_children[v][i++];
        lo[w] = counter + 1;
        walk.emplace_back(w, 0);
        continue;
      }
      h.label_[v] = ++counter;
      h.by_label_.push_back(v);
      walk.pop_back();
    }
  }
  // Interval sets, children before parents (decreasing component number).
  h.intervals_.assign(m, {});
  for (uint32_t c = m; c-- > 0;) {
    std::vector<Interval>& set = h.intervals_[c];
    set.push_back({lo[c], h.label_[c]});
    for (const uint32_t w : children[c]) {
      set.insert(set.end(), h.intervals_[w].begin(), h.intervals_[w].end());
    }
    Coalesce(&set);
    h.stats_.intervals += set.size();
    h.stats_.max_intervals = std::max(h.stats_.max_intervals, set.size());
  }
  size_t tree_edges = 0;
  for (uint32_t c = 0; c < m; c++) {
    tree_edges += tree_parent[c] == static_cast<uint32_t>(-1) ? 0 : 1;
  }
  h.stats_.non_tree_edges = h.stats_.edges - tree_edges;
  // Closure size: each component's descendants are the labels in its
  // intervals, each carrying its members.
  std::vector<size_t> weight(m + 1, 0);
  for (uint32_t label = 1; label <= m; label++) {
    weight[label] = weight[label - 1] + h.members_[h.by_label_[label - 1]].size();
  }
  for (uint32_t c = 0; c < m; c++) {
    size_t below = 0;
    for (const Interval& iv : h.intervals_[c]) {
      below += weight[iv.hi] - weight[iv.lo - 1];
    }
    h.stats_.closure_pairs += below * h.members_[c].size();
  }
  return h;
}

auto ClassHierarchy::Label(term_id_t cls) const -> uint32_t {
  const auto it = comp_of_.find(cls);
  return it == comp_of_.end() ? 0 : label_[it->second];
}

auto ClassHierarchy::Intervals(term_id_t cls) const -> const std::vector<Interval>& {
  const auto it = comp_of_.find(cls);
  return it == comp_of_.end() ? kNoIntervals : intervals_[it->second];
}

auto ClassHierarchy::ClassesAt(uint32_t label) const -> const std::vector<term_id_t>& {
  if (label == 0 || label > by_label_.size()) {
    return kNoClasses;
  }
  return members_[by_label_[label - 1]];
}

auto ClassHierarchy::Descendants(term_id_t cls) const -> std::vector<term_id_t> {
  std::vector<term_id_t> out;
  for (const Interval& iv : Intervals(cls)) {
    for (uint32_t label = iv.lo; label <= iv.hi; label++) {
      const auto& group = ClassesAt(label);
      out.insert(out.end(), group.begin(), group.end());
    }
  }
  return out;
}

auto ClassHierarchy::Ancestors(term_id_t cls) const -> std::vector<term_id_t> {
  const auto it = comp_of_.find(cls);
  if (it == comp_of_.end()) {
    return {};
  }
  std::vector<bool> seen(members_.size(), false);
  std::vector<uint32_t> todo{it->second};
  seen[it->second] = true;
  std::vector<term_id_t> out;
  while (!todo.empty()) {
    const uint32_t c = todo.back();
    todo.pop_back();
    out.insert(out.end(), members_[c].begin(), members_[c].end());
    for (const uint32_t p : parents_[c]) {
      if (!seen[p]) {
        seen[p] = true;
        todo.push_back(p);
      }
    }
  }
  return out;
}

auto ClassHierarchy::IsDescendant(term_id_t d, term_id_t c) const -> bool {
  const uint32_t label = Label(d);
  if (label == 0) {
    return false;
  }
  const auto& set = Intervals(c);
  const auto it = std::upper_bound(set.begin(), set.end(), label,
                                   [](uint32_t value, const Interval& iv) { return value < iv.lo; });
  return it != set.begin() && label <= (it - 1)->hi;
}

}  // namespace ontodb
