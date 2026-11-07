// The version index on its own: which triples each timestamp sees.

#include "concurrency/version_index.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace ontodb {
namespace {

const Triple kA{1, 2, 3};
const Triple kB{1, 2, 4};
const Triple kC{5, 2, 3};
const Triple kD{5, 6, 7};

auto Sorted(std::vector<Triple> triples) -> std::vector<Triple> {
  std::sort(triples.begin(), triples.end());
  return triples;
}

auto Insert(const Triple& t) -> VersionChange {
  return {true, t};
}

auto Delete(const Triple& t) -> VersionChange {
  return {false, t};
}

TEST(VersionIndex, NothingCommittedIsTimestampZero) {
  VersionIndex index;
  EXPECT_EQ(index.Clock(), 0U);
  EXPECT_EQ(index.Apply({}), 0U);
  EXPECT_TRUE(index.Scan({}, 0).empty());
}

TEST(VersionIndex, EachCommitSeesItselfAndEarlierTimestampsDoNot) {
  VersionIndex index;
  const auto one = index.Apply({Insert(kA)});
  const auto two = index.Apply({Insert(kB), Delete(kA)});
  EXPECT_EQ(one, 1U);
  EXPECT_EQ(two, 2U);
  EXPECT_FALSE(index.Contains(kA, 0));
  EXPECT_TRUE(index.Contains(kA, one));
  EXPECT_FALSE(index.Contains(kA, two));
  EXPECT_FALSE(index.Contains(kB, one));
  EXPECT_TRUE(index.Contains(kB, two));
  EXPECT_EQ(index.Count(one), 1U);
  EXPECT_EQ(index.Count(two), 1U);
}

TEST(VersionIndex, ATripleCanLeaveAndComeBack) {
  VersionIndex index;
  index.Apply({Insert(kA)});
  index.Apply({Delete(kA)});
  index.Apply({Insert(kB)});
  index.Apply({Insert(kA)});
  EXPECT_TRUE(index.Contains(kA, 1));
  EXPECT_FALSE(index.Contains(kA, 2));
  EXPECT_FALSE(index.Contains(kA, 3));
  EXPECT_TRUE(index.Contains(kA, 4));
  EXPECT_EQ(index.SpanCount(), 3U);
  EXPECT_EQ(index.TripleCount(), 2U);
  EXPECT_EQ(index.LastChange(kA), 4U);
}

TEST(VersionIndex, AddedAndRemovedInOneCommitLeavesNoSpan) {
  VersionIndex index;
  EXPECT_EQ(index.Apply({Insert(kA), Delete(kA)}), 1U);
  EXPECT_FALSE(index.Contains(kA, 1));
  EXPECT_EQ(index.SpanCount(), 0U);
  EXPECT_EQ(index.LastChange(kA), 0U);
}

TEST(VersionIndex, RemovedAndAddedInOneCommitSplitsTheSpan) {
  VersionIndex index;
  index.Apply({Insert(kA)});
  index.Apply({Delete(kA), Insert(kA)});
  EXPECT_TRUE(index.Contains(kA, 1));
  EXPECT_TRUE(index.Contains(kA, 2));
  EXPECT_EQ(index.LastChange(kA), 2U);
}

TEST(VersionIndex, LastChangeSeesARemoval) {
  VersionIndex index;
  EXPECT_EQ(index.LastChange(kA), 0U);
  index.Apply({Insert(kA)});
  index.Apply({Insert(kB)});
  EXPECT_EQ(index.LastChange(kA), 1U);
  index.Apply({Delete(kA)});
  EXPECT_EQ(index.LastChange(kA), 3U);
}

TEST(VersionIndex, EveryPatternShapeFindsWhatAFullScanFinds) {
  VersionIndex index;
  std::mt19937 random(7);
  std::uniform_int_distribution<term_id_t> id(1, 6);
  std::set<Triple> live;
  std::vector<std::set<Triple>> states;
  for (int commit = 0; commit < 60; commit++) {
    std::vector<VersionChange> changes;
    for (int i = 0; i < 4; i++) {
      const Triple t{id(random), id(random), id(random)};
      if (live.contains(t)) {
        live.erase(t);
        changes.push_back(Delete(t));
      } else {
        live.insert(t);
        changes.push_back(Insert(t));
      }
    }
    index.Apply(changes);
    states.push_back(live);
  }
  std::vector<std::optional<term_id_t>> options = {std::nullopt, 1, 2, 3, 4, 5, 6, 99};
  for (commit_ts_t ts = 1; ts <= states.size(); ts += 7) {
    const auto& state = states[ts - 1];
    EXPECT_EQ(index.Count(ts), state.size());
    for (const auto& s : options) {
      for (const auto& p : options) {
        for (const auto& o : options) {
          const TriplePattern pattern{s, p, o};
          std::vector<Triple> expected;
          std::copy_if(state.begin(), state.end(), std::back_inserter(expected),
                       [&](const Triple& t) { return Matches(t, pattern); });
          ASSERT_EQ(Sorted(index.Scan(pattern, ts)), expected);
        }
      }
    }
  }
}

TEST(VersionIndex, ATagNamesOneTimestampForGood) {
  VersionIndex index;
  index.Apply({Insert(kA)});
  EXPECT_EQ(index.Tag("0.1.0"), 1U);
  EXPECT_EQ(index.Tag("0.1.0"), 1U);  // the same name for the same timestamp is no change
  index.Apply({Insert(kB)});
  EXPECT_THROW(index.Tag("0.1.0"), std::invalid_argument);
  EXPECT_EQ(index.FindTag("0.1.0"), 1U);
  EXPECT_EQ(index.FindTag("nothing"), std::nullopt);
  index.Tag("0.2.0");
  index.Tag("first", 1);
  const auto tags = index.Tags();
  ASSERT_EQ(tags.size(), 3U);
  EXPECT_EQ(tags[0].name, "0.1.0");
  EXPECT_EQ(tags[1].name, "first");
  EXPECT_EQ(tags[2].name, "0.2.0");
}

TEST(VersionIndex, ABadTagIsRefused) {
  VersionIndex index;
  EXPECT_THROW(index.Tag(""), std::invalid_argument);
  EXPECT_THROW(index.Tag("two words"), std::invalid_argument);
  EXPECT_THROW(index.Tag("later", 5), std::invalid_argument);
}

TEST(VersionIndex, DumpAndRestoreKeepEverything) {
  VersionIndex index;
  index.Apply({Insert(kA), Insert(kB)});
  index.Tag("one");
  index.Apply({Delete(kA), Insert(kC)});
  index.Apply({Insert(kA), Delete(kB)});
  VersionIndex copy;
  copy.Restore(index.Dump(), index.Tags(), index.Clock());
  EXPECT_EQ(copy.Clock(), 3U);
  EXPECT_EQ(copy.FindTag("one"), 1U);
  for (commit_ts_t ts = 0; ts <= 3; ts++) {
    EXPECT_EQ(Sorted(copy.Scan({}, ts)), Sorted(index.Scan({}, ts)));
  }
  EXPECT_EQ(copy.Scan(TriplePattern{std::nullopt, 2, 3}, 2),
            std::vector<Triple>{kC});  // the secondary orderings came back too
  EXPECT_EQ(copy.Apply({Insert(kD)}), 4U);
}

TEST(VersionIndex, RemapKeepsTheHistoryUnderNewIds) {
  VersionIndex index;
  index.Apply({Insert(kA)});
  index.Apply({Delete(kA)});
  index.Tag("gone");
  index.Remap([](const Triple& t) {
    return Triple{t.subject + 10, t.predicate + 10, t.object + 10};
  });
  const Triple moved{11, 12, 13};
  EXPECT_TRUE(index.Contains(moved, 1));
  EXPECT_FALSE(index.Contains(moved, 2));
  EXPECT_FALSE(index.Contains(kA, 1));
  EXPECT_EQ(index.Scan(TriplePattern{std::nullopt, 12, std::nullopt}, 1),
            std::vector<Triple>{moved});
  EXPECT_EQ(index.FindTag("gone"), 2U);
}

// One transaction moves a triple between two places. A reader at any timestamp must see it in
// exactly one, or a commit became visible part by part.
TEST(VersionIndex, AReaderNeverSeesPartOfACommit) {
  VersionIndex index;
  const Triple here{1, 1, 1};
  const Triple there{2, 2, 2};
  index.Apply({Insert(here)});
  std::atomic<bool> done{false};
  std::thread writer([&] {
    bool at_here = true;
    for (int i = 0; i < 20000; i++) {
      index.Apply({Delete(at_here ? here : there), Insert(at_here ? there : here)});
      at_here = !at_here;
    }
    done = true;
  });
  size_t checked = 0;
  while (!done) {
    const commit_ts_t ts = index.Clock();
    EXPECT_EQ(index.Scan({}, ts).size(), 1U) << ts;
    checked++;
  }
  writer.join();
  EXPECT_GT(checked, 0U);
}

}  // namespace
}  // namespace ontodb
