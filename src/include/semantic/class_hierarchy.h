#pragma once

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/types.h"

namespace ontodb {

// An inclusive range of postorder labels, both at least 1.
struct Interval {
  uint32_t lo{0};
  uint32_t hi{0};

  friend auto operator==(const Interval& a, const Interval& b) -> bool {
    return a.lo == b.lo && a.hi == b.hi;
  }
};

// A class hierarchy (rdfs:subClassOf) labelled so that "everything below C"
// is a few ranges of integers (docs/semantic-search.md, section 3).
//
// Build takes (class, parent) pairs. Classes that sit on a cycle denote the
// same class and are collapsed: they share one label and one set of
// intervals. The classes of the condensed graph are numbered in postorder of
// a spanning forest, and each gets a set of intervals: its own tree interval
// merged with the intervals of every child (Agrawal, Borgida, Jagadish,
// SIGMOD 1989). Then d is at or below c exactly when d's label lies in one of
// c's intervals. For a tree every class has one interval; a class has at most
// 1 + (the non-tree edges below it) intervals.
//
// Everything here is reflexive: a class is below and above itself. The object
// is immutable after Build and safe to read from several threads.
class ClassHierarchy {
 public:
  // How a class with several parents picks the parent that carries it in the
  // spanning forest. kDeepest takes the parent with the longest path from a
  // root (ties: the smaller id); kFirst takes the first parent listed.
  enum class TreeParent { kDeepest, kFirst };

  struct Stats {
    size_t classes{0};         // distinct term ids
    size_t components{0};      // classes after collapsing cycles
    size_t edges{0};           // distinct edges between components
    size_t non_tree_edges{0};  // edges outside the spanning forest
    size_t intervals{0};       // total over all components
    size_t max_intervals{0};   // the largest set
    size_t closure_pairs{0};   // pairs (descendant, ancestor), reflexive, over terms
  };

  // `isolated` lists classes that appear in no edge (a class with neither a
  // parent nor a subclass); each becomes a root with one label.
  static auto Build(const std::vector<std::pair<term_id_t, term_id_t>>& edges,
                    TreeParent rule = TreeParent::kDeepest,
                    const std::vector<term_id_t>& isolated = {}) -> ClassHierarchy;

  auto Contains(term_id_t cls) const -> bool { return comp_of_.count(cls) != 0; }
  // The postorder label of cls, or 0 when cls is not in the hierarchy.
  auto Label(term_id_t cls) const -> uint32_t;
  // The intervals of cls, sorted and disjoint; empty when cls is absent.
  auto Intervals(term_id_t cls) const -> const std::vector<Interval>&;
  // Every class at or below `cls`, in label order; empty when absent.
  auto Descendants(term_id_t cls) const -> std::vector<term_id_t>;
  // Every class at or above `cls`; empty when absent.
  auto Ancestors(term_id_t cls) const -> std::vector<term_id_t>;
  auto IsDescendant(term_id_t d, term_id_t c) const -> bool;
  // The classes that carry a label (several only for a collapsed cycle).
  auto ClassesAt(uint32_t label) const -> const std::vector<term_id_t>&;
  auto MaxLabel() const -> uint32_t { return static_cast<uint32_t>(by_label_.size()); }
  auto GetStats() const -> const Stats& { return stats_; }

 private:
  std::unordered_map<term_id_t, uint32_t> comp_of_;       // term id to component
  std::vector<std::vector<term_id_t>> members_;           // component to its term ids
  std::vector<std::vector<uint32_t>> parents_;            // component to parent components
  std::vector<uint32_t> label_;                           // component to postorder label
  std::vector<uint32_t> by_label_;                        // label - 1 to component
  std::vector<std::vector<Interval>> intervals_;          // component to intervals
  Stats stats_;
};

}  // namespace ontodb
