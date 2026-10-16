#include "optimizer/optimizer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "binder/binder.h"
#include "common/database.h"
#include "common/exception.h"
#include "dictionary/mem_dictionary.h"
#include "execution/executor_factory.h"
#include "optimizer/cardinality_estimator.h"
#include "parser/parser.h"
#include "planner/planner.h"
#include "stats/statistics.h"
#include "store/mem_store.h"
#include "test_util.h"

namespace ontodb {
namespace {

auto CollectRows(const AbstractPlanNode* plan, ExecutorContext* ctx) -> std::vector<Row> {
  auto exec = ExecutorFactory::Create(plan, ctx);
  exec->Init();
  std::vector<Row> rows;
  Row row;
  while (exec->Next(&row)) {
    rows.push_back(row);
  }
  std::sort(rows.begin(), rows.end());
  return rows;
}

void Walk(const AbstractPlanNode* node, std::vector<PlanType>* types) {
  types->push_back(node->GetType());
  for (const auto& child : node->GetChildren()) {
    Walk(child.get(), types);
  }
}

TEST(P5_1_Statistics, CountsPositionsAndPatterns) {
  MemDictionary dict;
  MemStore store;
  store.Insert({1, 10, 100});
  store.Insert({1, 10, 101});
  store.Insert({2, 10, 100});
  store.Insert({2, 11, 100});
  Statistics stats;
  stats.Collect(store, dict);
  EXPECT_EQ(stats.TripleCount(), 4u);
  EXPECT_EQ(stats.DistinctCount("s"), 2u);
  EXPECT_EQ(stats.DistinctCount("p"), 2u);
  EXPECT_EQ(stats.DistinctCount("o"), 2u);
  EXPECT_EQ(stats.PredicateCount(10), 3u);
  EXPECT_EQ(stats.PredicateCount(11), 1u);
  TriplePattern subject_one;
  subject_one.subject = 1;
  EXPECT_EQ(stats.PatternCount(subject_one), 2u);
  TriplePattern pred_obj;
  pred_obj.predicate = 10;
  pred_obj.object = 100;
  EXPECT_EQ(stats.PatternCount(pred_obj), 2u);
  // (1, 11), (11, 100), and (1, 100) each occur in some triple, but
  // (1, 11, 100) does not: pair counts cannot answer the fully bound pattern.
  store.Insert({1, 11, 105});
  stats.Collect(store, dict);
  EXPECT_GT(stats.PatternCount({1, 11, std::nullopt}), 0u);
  EXPECT_GT(stats.PatternCount({std::nullopt, 11, 100}), 0u);
  EXPECT_GT(stats.PatternCount({1, std::nullopt, 100}), 0u);
  EXPECT_EQ(stats.PatternCount({1, 11, 100}), 0u);
  EXPECT_EQ(stats.PatternCount({2, 11, 100}), 1u);
}

TEST(P5_2_Cardinality, MonotoneAndNonNegative) {
  MemDictionary dict;
  MemStore store;
  store.Insert({1, 10, 100});
  store.Insert({1, 10, 101});
  store.Insert({2, 10, 100});
  store.Insert({2, 11, 100});
  Statistics stats;
  stats.Collect(store, dict);
  CardinalityEstimator estimator(&stats);
  BoundTriplePattern all;
  all.subject = BoundTerm{true, 0, INVALID_TERM_ID, {}};
  all.predicate = BoundTerm{true, 1, INVALID_TERM_ID, {}};
  all.object = BoundTerm{true, 2, INVALID_TERM_ID, {}};
  const double unbound = estimator.EstimateTriplePattern(all);
  EXPECT_TRUE(std::isfinite(unbound));
  EXPECT_DOUBLE_EQ(unbound, static_cast<double>(stats.TripleCount()));
  BoundTriplePattern pred = all;
  pred.predicate = BoundTerm{false, 0, 10, "<p>"};
  const double one = estimator.EstimateTriplePattern(pred);
  EXPECT_TRUE(std::isfinite(one));
  EXPECT_GE(one, 0);
  EXPECT_LE(one, unbound);
  BoundTriplePattern both = pred;
  both.object = BoundTerm{false, 0, 100, "<o>"};
  const double two = estimator.EstimateTriplePattern(both);
  EXPECT_TRUE(std::isfinite(two));
  EXPECT_GE(two, 0);
  EXPECT_LE(two, one);
  const double join = estimator.EstimateJoin(unbound, one, 1);
  EXPECT_TRUE(std::isfinite(join));
  EXPECT_GE(join, 0);
}

// 100 students, each with a name and 3 of 10 courses. A star join on the
// subject and a join from takes to the course class both come out exact with
// distinct counts; min(l, r) gets the second wrong by 30x.
TEST(P5_2_Cardinality, DistinctValuesGetStarAndClassJoinsRight) {
  MemDictionary dict;
  MemStore store;
  const term_id_t type = 1;
  const term_id_t name = 2;
  const term_id_t takes = 3;
  const term_id_t student = 4;
  const term_id_t course = 5;
  for (term_id_t c = 0; c < 10; c++) {
    store.Insert({1000 + c, type, course});
  }
  for (term_id_t s = 0; s < 100; s++) {
    store.Insert({s + 10, type, student});
    store.Insert({s + 10, name, 5000 + s});
    for (term_id_t k = 0; k < 3; k++) {
      store.Insert({s + 10, takes, 1000 + (s + k) % 10});
    }
  }
  Statistics stats;
  stats.Collect(store, dict);
  CardinalityEstimator estimator(&stats);
  auto var = [](size_t slot) { return BoundTerm{true, slot, INVALID_TERM_ID, {}}; };
  auto id = [](term_id_t value) { return BoundTerm{false, 0, value, "c"}; };
  const BoundTriplePattern students{var(0), id(type), id(student), ""};
  const BoundTriplePattern names{var(0), id(name), var(1), ""};
  const BoundTriplePattern enrolled{var(0), id(takes), var(2), ""};
  const BoundTriplePattern courses{var(2), id(type), id(course), ""};
  auto join = [&](const BoundTriplePattern& l, const BoundTriplePattern& r, size_t slot) {
    return estimator.EstimateJoinWith(
        estimator.EstimateTriplePattern(l), estimator.EstimateTriplePattern(r),
        estimator.EstimateDistinct(l, slot), estimator.EstimateDistinct(r, slot));
  };
  EXPECT_DOUBLE_EQ(join(students, names, 0), 100);           // truth: 100
  EXPECT_DOUBLE_EQ(join(enrolled, courses, 2), 300);         // truth: 300
  EXPECT_DOUBLE_EQ(estimator.EstimateJoin(300, 10, 1), 10);  // the fallback's guess
}

TEST(P5_3_JoinOrder, PreservesResults) {
  MemDictionary dict;
  MemStore store;
  const auto a = dict.Insert("<http://ex/a>");
  const auto b = dict.Insert("<http://ex/b>");
  const auto p = dict.Insert("<http://ex/p>");
  const auto q = dict.Insert("<http://ex/q>");
  const auto r = dict.Insert("<http://ex/r>");
  store.Insert({a, p, b});
  store.Insert({b, q, a});
  store.Insert({a, r, a});
  const char* query =
      "SELECT ?a ?c WHERE { ?a <http://ex/p> ?b . ?b <http://ex/q> ?c . ?a <http://ex/r> ?c . }";
  const auto bound = Binder(dict).Bind(Parser(query).Parse());
  auto plan = Planner().Plan(bound);
  Statistics stats;
  stats.Collect(store, dict);
  CardinalityEstimator estimator(&stats);
  Config config;
  config.join = JoinMethod::kNestedLoop;
  Optimizer optimizer(&estimator, &config);
  auto rewritten = optimizer.Optimize(std::move(plan));
  Config exec_config;
  IoStats io;
  NullTraceSink trace;
  ExecutorContext ctx(&store, &dict, &exec_config, nullptr, nullptr, nullptr, nullptr, &io, &trace,
                      nullptr);
  auto original = Planner().Plan(bound);
  EXPECT_EQ(CollectRows(rewritten.get(), &ctx), CollectRows(original.get(), &ctx));
}

// Written in the worst order, the unbound pattern first; the optimizer puts
// the selective type pattern first, and the answer does not change.
TEST(P5_3_JoinOrder, SelectivePatternGoesFirst) {
  Database db;
  db.Load(test::DataFile("tiny.ttl").string());
  const std::string query =
      "PREFIX ub: <http://example.edu/univ#> "
      "SELECT ?s ?p ?o WHERE { ?s ?p ?o . ?s a ub:Professor . }";
  const auto plan = db.Explain(query);
  const auto first = plan.find("TripleScan");
  ASSERT_NE(first, std::string::npos) << plan;
  EXPECT_NE(plan.substr(first, plan.find('\n', first) - first).find("Professor"), std::string::npos)
      << plan;
  EXPECT_FALSE(db.Execute(query, nullptr).rows.empty());
}

TEST(P5_4_JoinAlgorithm, KnobSelectsTheAlgorithm) {
  MemDictionary dict;
  MemStore store;
  store.Insert({1, 2, 3});
  const auto bound = Binder(dict).Bind(
      Parser("SELECT ?s ?o WHERE { ?s <http://ex/p> ?o . ?s <http://ex/q> ?o . }").Parse());
  Statistics stats;
  stats.Collect(store, dict);
  CardinalityEstimator estimator(&stats);
  Config config;
  config.join = JoinMethod::kHash;
  Optimizer hash_opt(&estimator, &config);
  auto hashed = hash_opt.Optimize(Planner().Plan(bound));
  std::vector<PlanType> types;
  Walk(hashed.get(), &types);
  EXPECT_NE(std::find(types.begin(), types.end(), PlanType::kHashJoin), types.end());
  config.join = JoinMethod::kNestedLoop;
  Optimizer nlj_opt(&estimator, &config);
  auto looped = nlj_opt.Optimize(Planner().Plan(bound));
  types.clear();
  Walk(looped.get(), &types);
  EXPECT_EQ(std::find(types.begin(), types.end(), PlanType::kHashJoin), types.end());
  EXPECT_NE(std::find(types.begin(), types.end(), PlanType::kNestedLoopJoin), types.end());
}

// ?a starts (10 rows). ?a p ?b adds 5 rows each (50). ?b q ?c has 20 rows:
// the outer side (50) is now larger than the inner pattern, so auto builds a
// hash table on it; the first join, with 10 outer rows, probes the index.
TEST(P5_4_JoinAlgorithm, AutoProbesSmallOuterHashesLargeOuter) {
  MemDictionary dict;
  MemStore store;
  const auto t = dict.Insert("<http://ex/t>");
  const auto c = dict.Insert("<http://ex/C>");
  const auto p = dict.Insert("<http://ex/p>");
  const auto q = dict.Insert("<http://ex/q>");
  for (term_id_t a = 0; a < 10; a++) {
    store.Insert({100 + a, t, c});
    for (term_id_t k = 0; k < 5; k++) {
      store.Insert({100 + a, p, 200 + (a * 5 + k) % 20});
    }
  }
  for (term_id_t b = 0; b < 20; b++) {
    store.Insert({200 + b, q, 300 + b});
  }
  const auto bound =
      Binder(dict).Bind(Parser("SELECT * WHERE { ?b <http://ex/q> ?c . "
                               "?a <http://ex/p> ?b . ?a <http://ex/t> <http://ex/C> . }")
                            .Parse());
  Statistics stats;
  stats.Collect(store, dict);
  CardinalityEstimator estimator(&stats);
  Config config;
  config.join = JoinMethod::kAuto;
  auto plan = Optimizer(&estimator, &config).Optimize(Planner().Plan(bound));
  std::vector<PlanType> types;
  Walk(plan.get(), &types);
  // Pre-order from the top: the last join is the hash join, the one under it probes.
  const auto hash = std::find(types.begin(), types.end(), PlanType::kHashJoin);
  const auto probe = std::find(types.begin(), types.end(), PlanType::kIndexNestedLoopJoin);
  ASSERT_NE(hash, types.end()) << plan->ToString();
  ASSERT_NE(probe, types.end()) << plan->ToString();
  EXPECT_LT(hash, probe) << plan->ToString();
  Config exec;
  IoStats io;
  NullTraceSink trace;
  ExecutorContext ctx(&store, &dict, &exec, nullptr, nullptr, nullptr, nullptr, &io, &trace,
                      nullptr);
  EXPECT_EQ(CollectRows(plan.get(), &ctx).size(), 50u);
}

TEST(P5_5_ExplainAnalyze, PrintsEstimateAndActual) {
  Database db;
  db.Load(test::DataFile("tiny.ttl").string());
  const auto text = db.ExplainAnalyze(
      "PREFIX ub: <http://example.edu/univ#> SELECT ?name WHERE { ?s ub:name ?name . }");
  std::string lower = text;
  for (char& c : lower) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  EXPECT_NE(lower.find("actual"), std::string::npos) << text;
}

}  // namespace
}  // namespace ontodb
