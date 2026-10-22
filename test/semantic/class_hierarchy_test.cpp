#include "semantic/class_hierarchy.h"

#include <algorithm>
#include <map>
#include <random>
#include <set>

#include <gtest/gtest.h>

namespace ontodb {
namespace {

using Edges = std::vector<std::pair<term_id_t, term_id_t>>;

// Reflexive transitive closure by search, the oracle.
auto NaiveBelow(const Edges& edges, term_id_t c) -> std::set<term_id_t> {
  std::map<term_id_t, std::vector<term_id_t>> children;
  for (const auto& [child, parent] : edges) {
    children[parent].push_back(child);
  }
  std::set<term_id_t> seen{c};
  std::vector<term_id_t> todo{c};
  while (!todo.empty()) {
    const term_id_t v = todo.back();
    todo.pop_back();
    for (const term_id_t w : children[v]) {
      if (seen.insert(w).second) {
        todo.push_back(w);
      }
    }
  }
  return seen;
}

auto AsSet(const std::vector<term_id_t>& v) -> std::set<term_id_t> {
  return {v.begin(), v.end()};
}

TEST(P14_1_ClassHierarchy, TreeHasOneIntervalPerClass) {
  // 1 is the root; 2, 3 under 1; 4, 5 under 2; 6 under 3.
  const Edges edges{{2, 1}, {3, 1}, {4, 2}, {5, 2}, {6, 3}};
  const auto h = ClassHierarchy::Build(edges);
  EXPECT_EQ(h.GetStats().classes, 6u);
  EXPECT_EQ(h.GetStats().non_tree_edges, 0u);
  for (term_id_t c = 1; c <= 6; c++) {
    EXPECT_EQ(h.Intervals(c).size(), 1u) << c;
  }
  EXPECT_EQ(AsSet(h.Descendants(2)), (std::set<term_id_t>{2, 4, 5}));
  EXPECT_EQ(AsSet(h.Descendants(1)), (std::set<term_id_t>{1, 2, 3, 4, 5, 6}));
  EXPECT_EQ(h.Descendants(6), (std::vector<term_id_t>{6}));
  EXPECT_EQ(AsSet(h.Ancestors(5)), (std::set<term_id_t>{5, 2, 1}));
  // Labels are a permutation of 1..n with a node after everything below it.
  std::set<uint32_t> labels;
  for (term_id_t c = 1; c <= 6; c++) {
    labels.insert(h.Label(c));
  }
  EXPECT_EQ(labels, (std::set<uint32_t>{1, 2, 3, 4, 5, 6}));
  EXPECT_EQ(h.Label(1), 6u);
}

TEST(P14_1_ClassHierarchy, DiamondNeedsTwoIntervalsForTheTop) {
  // 4 below both 2 and 3, which are below 1; plus a leaf 5 under 3 to split the labels.
  const Edges edges{{2, 1}, {3, 1}, {4, 2}, {4, 3}, {5, 3}};
  const auto h = ClassHierarchy::Build(edges);
  EXPECT_EQ(h.GetStats().non_tree_edges, 1u);
  EXPECT_EQ(AsSet(h.Descendants(2)), (std::set<term_id_t>{2, 4}));
  EXPECT_EQ(AsSet(h.Descendants(3)), (std::set<term_id_t>{3, 4, 5}));
  EXPECT_TRUE(h.IsDescendant(4, 2));
  EXPECT_TRUE(h.IsDescendant(4, 3));
  EXPECT_FALSE(h.IsDescendant(5, 2));
  EXPECT_TRUE(h.IsDescendant(1, 1));
  EXPECT_FALSE(h.IsDescendant(1, 2));
}

TEST(P14_1_ClassHierarchy, AbsentClassesAnswerEmpty) {
  const auto h = ClassHierarchy::Build({{2, 1}});
  EXPECT_FALSE(h.Contains(9));
  EXPECT_EQ(h.Label(9), 0u);
  EXPECT_TRUE(h.Intervals(9).empty());
  EXPECT_TRUE(h.Descendants(9).empty());
  EXPECT_TRUE(h.Ancestors(9).empty());
  EXPECT_FALSE(h.IsDescendant(9, 1));
  EXPECT_FALSE(h.IsDescendant(1, 9));
}

TEST(P14_1_ClassHierarchy, CycleCollapsesToOneClass) {
  // 1 and 2 are each other's subclass; 3 is below 2; 4 is above 1.
  const Edges edges{{1, 2}, {2, 1}, {3, 2}, {1, 4}};
  const auto h = ClassHierarchy::Build(edges);
  EXPECT_EQ(h.GetStats().classes, 4u);
  EXPECT_EQ(h.GetStats().components, 3u);
  EXPECT_EQ(h.Label(1), h.Label(2));
  EXPECT_EQ(AsSet(h.Descendants(1)), (std::set<term_id_t>{1, 2, 3}));
  EXPECT_EQ(AsSet(h.Descendants(2)), (std::set<term_id_t>{1, 2, 3}));
  EXPECT_EQ(AsSet(h.Descendants(4)), (std::set<term_id_t>{1, 2, 3, 4}));
  EXPECT_EQ(AsSet(h.Ancestors(3)), (std::set<term_id_t>{3, 1, 2, 4}));
  EXPECT_TRUE(h.IsDescendant(2, 1));
  EXPECT_TRUE(h.IsDescendant(1, 2));
}

TEST(P14_1_ClassHierarchy, SelfLoopsAndDuplicateEdgesAreIgnored) {
  const auto h = ClassHierarchy::Build({{2, 1}, {2, 1}, {1, 1}, {3, 3}});
  EXPECT_EQ(h.GetStats().edges, 1u);
  EXPECT_EQ(AsSet(h.Descendants(1)), (std::set<term_id_t>{1, 2}));
  EXPECT_EQ(h.Descendants(3), (std::vector<term_id_t>{3}));
}

TEST(P14_1_ClassHierarchy, RandomDagsMatchTheTransitiveClosure) {
  std::mt19937_64 rng(7);
  for (int round = 0; round < 60; round++) {
    const int n = 3 + static_cast<int>(rng() % 40);
    Edges edges;
    // Edges only from a larger id to a smaller one keep it acyclic; ids are
    // shuffled so the order is not the numbering.
    std::vector<term_id_t> ids(n);
    for (int i = 0; i < n; i++) {
      ids[i] = 100 + static_cast<term_id_t>(i) * 7;
    }
    std::shuffle(ids.begin(), ids.end(), rng);
    for (int i = 1; i < n; i++) {
      const int parents = 1 + static_cast<int>(rng() % 3);
      for (int k = 0; k < parents; k++) {
        edges.emplace_back(ids[i], ids[rng() % i]);
      }
    }
    std::shuffle(edges.begin(), edges.end(), rng);
    for (const auto rule : {ClassHierarchy::TreeParent::kDeepest, ClassHierarchy::TreeParent::kFirst}) {
      const auto h = ClassHierarchy::Build(edges, rule);
      size_t pairs = 0;
      for (const term_id_t c : ids) {
        const auto below = NaiveBelow(edges, c);
        pairs += below.size();
        ASSERT_EQ(AsSet(h.Descendants(c)), below) << "round " << round << " class " << c;
        for (const term_id_t d : ids) {
          ASSERT_EQ(h.IsDescendant(d, c), below.count(d) == 1) << d << " under " << c;
        }
        // Ancestors are the classes that have c below them.
        std::set<term_id_t> above;
        for (const term_id_t a : ids) {
          if (NaiveBelow(edges, a).count(c) == 1) {
            above.insert(a);
          }
        }
        ASSERT_EQ(AsSet(h.Ancestors(c)), above);
        // Intervals are sorted, disjoint, and not adjacent.
        const auto& set = h.Intervals(c);
        for (size_t i = 1; i < set.size(); i++) {
          ASSERT_GT(set[i].lo, set[i - 1].hi + 1);
        }
        ASSERT_LE(set.size(), 1 + h.GetStats().non_tree_edges);
      }
      EXPECT_EQ(h.GetStats().closure_pairs, pairs);
    }
  }
}

TEST(P14_1_ClassHierarchy, LabelsDoNotDependOnEdgeOrder) {
  Edges edges{{2, 1}, {3, 1}, {4, 2}, {4, 3}, {5, 3}, {6, 5}};
  const auto a = ClassHierarchy::Build(edges);
  std::reverse(edges.begin(), edges.end());
  const auto b = ClassHierarchy::Build(edges);
  for (term_id_t c = 1; c <= 6; c++) {
    EXPECT_EQ(a.Label(c), b.Label(c)) << c;
    EXPECT_EQ(a.Intervals(c), b.Intervals(c)) << c;
  }
}

}  // namespace
}  // namespace ontodb
