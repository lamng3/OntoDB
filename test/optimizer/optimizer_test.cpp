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

TEST(P5_1_Statistics, DISABLED_CountsPositionsAndPatterns) {
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
}

TEST(P5_2_Cardinality, DISABLED_MonotoneAndNonNegative) {
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

TEST(P5_3_JoinOrder, DISABLED_PreservesResults) {
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

TEST(P5_4_JoinAlgorithm, DISABLED_KnobSelectsTheAlgorithm) {
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

TEST(P5_5_ExplainAnalyze, DISABLED_PrintsEstimateAndActual) {
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
