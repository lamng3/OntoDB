#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "common/exception.h"
#include "dictionary/mem_dictionary.h"
#include "execution/executor_factory.h"
#include "execution/executors/nested_loop_join_executor.h"
#include "execution/executors/triple_scan_executor.h"
#include "execution/plans/nested_loop_join_plan.h"
#include "execution/plans/projection_plan.h"
#include "execution/plans/stub_plans.h"
#include "execution/plans/triple_scan_plan.h"
#include "store/mem_store.h"

namespace ontodb {
namespace {

class VecIter : public TripleIterator {
 public:
  explicit VecIter(std::vector<Triple> triples) : triples_(std::move(triples)) {}
  void Init() override { index_ = 0; }
  auto Next(Triple* out) -> bool override {
    if (index_ >= triples_.size()) {
      return false;
    }
    *out = triples_[index_++];
    return true;
  }

 private:
  std::vector<Triple> triples_;
  size_t index_{0};
};

class RecordingStore : public TripleStore {
 public:
  std::vector<Triple> triples;
  std::vector<TriplePattern> scans;

  auto Insert(const Triple& triple) -> bool override {
    triples.push_back(triple);
    return true;
  }
  auto Delete(const Triple&) -> bool override { return false; }
  auto Scan(const TriplePattern& pattern) -> std::unique_ptr<TripleIterator> override {
    scans.push_back(pattern);
    std::vector<Triple> hit;
    for (const auto& triple : triples) {
      if (Matches(triple, pattern)) {
        hit.push_back(triple);
      }
    }
    return std::make_unique<VecIter>(std::move(hit));
  }
};

auto BoundVar(size_t slot) -> BoundTerm {
  BoundTerm term;
  term.is_variable = true;
  term.slot = slot;
  term.text = "v";
  return term;
}

auto BoundId(term_id_t id) -> BoundTerm {
  BoundTerm term;
  term.is_variable = false;
  term.id = id;
  term.text = "c";
  return term;
}

auto RowsOf(AbstractExecutor* exec) -> std::vector<Row> {
  exec->Init();
  std::vector<Row> rows;
  Row row;
  while (exec->Next(&row)) {
    rows.push_back(row);
  }
  std::sort(rows.begin(), rows.end());
  return rows;
}

TEST(P4_1_Rebind, DISABLED_InnerScanSeesOuterBinding) {
  RecordingStore store;
  store.triples = {{10, 1, 100}, {10, 2, 200}, {10, 2, 201}, {20, 1, 100}, {20, 2, 300}};
  Config config;
  IoStats stats;
  NullTraceSink trace;
  ExecutorContext ctx(&store, nullptr, &config, nullptr, nullptr, nullptr, nullptr, &stats, &trace,
                      nullptr);
  Schema schema{{{"s", 0}, {"o", 1}, {"x", 2}}};
  BoundTriplePattern left;
  left.subject = BoundVar(0);
  left.predicate = BoundId(1);
  left.object = BoundVar(1);
  BoundTriplePattern right;
  right.subject = BoundVar(0);
  right.predicate = BoundId(2);
  right.object = BoundVar(2);
  auto left_plan = std::make_unique<TripleScanPlan>(schema, left, 3);
  auto right_plan = std::make_unique<TripleScanPlan>(schema, right, 3);
  auto join_plan = std::make_unique<NestedLoopJoinPlan>(schema, nullptr, nullptr, 3);
  NestedLoopJoinExecutor join(std::make_unique<TripleScanExecutor>(&ctx, left_plan.get()),
                              std::make_unique<TripleScanExecutor>(&ctx, right_plan.get()),
                              join_plan.get());
  (void)RowsOf(&join);
  std::set<term_id_t> bound_subjects;
  for (const auto& pattern : store.scans) {
    if (pattern.predicate == term_id_t{2} && pattern.subject.has_value()) {
      bound_subjects.insert(*pattern.subject);
    }
  }
  EXPECT_TRUE(bound_subjects.contains(10));
  EXPECT_TRUE(bound_subjects.contains(20));
}

auto ScanPlan(term_id_t predicate, size_t width) -> std::unique_ptr<TripleScanPlan> {
  Schema schema;
  for (size_t i = 0; i < width; i++) {
    schema.columns.push_back({"c" + std::to_string(i), static_cast<uint32_t>(i)});
  }
  BoundTriplePattern pattern;
  pattern.subject = BoundVar(0);
  pattern.predicate = BoundId(predicate);
  pattern.object = BoundVar(1);
  return std::make_unique<TripleScanPlan>(schema, pattern, width);
}

TEST(P4_2_IndexNLJ, DISABLED_SameRowsAsNestedLoop) {
  MemStore store;
  store.Insert({10, 1, 100});
  store.Insert({10, 2, 200});
  store.Insert({20, 1, 100});
  store.Insert({20, 2, 300});
  Config config;
  IoStats stats;
  NullTraceSink trace;
  ExecutorContext ctx(&store, nullptr, &config, nullptr, nullptr, nullptr, nullptr, &stats, &trace,
                      nullptr);
  Schema schema{{{"s", 0}, {"o", 1}, {"x", 2}}};
  BoundTriplePattern left_pattern;
  left_pattern.subject = BoundVar(0);
  left_pattern.predicate = BoundId(1);
  left_pattern.object = BoundVar(1);
  BoundTriplePattern right_pattern;
  right_pattern.subject = BoundVar(0);
  right_pattern.predicate = BoundId(2);
  right_pattern.object = BoundVar(2);
  auto left = std::make_unique<TripleScanPlan>(schema, left_pattern, 3);
  auto right = std::make_unique<TripleScanPlan>(schema, right_pattern, 3);
  auto nlj_plan = std::make_unique<NestedLoopJoinPlan>(schema, nullptr, nullptr, 3);
  NestedLoopJoinExecutor nlj(std::make_unique<TripleScanExecutor>(&ctx, left.get()),
                             std::make_unique<TripleScanExecutor>(&ctx, right.get()),
                             nlj_plan.get());
  const auto expect = RowsOf(&nlj);
  auto inlj_plan = std::make_unique<IndexNestedLoopJoinPlan>(
      schema, std::make_unique<TripleScanPlan>(schema, left_pattern, 3),
      std::make_unique<TripleScanPlan>(schema, right_pattern, 3));
  auto inlj = ExecutorFactory::Create(inlj_plan.get(), &ctx);
  EXPECT_EQ(RowsOf(inlj.get()), expect);
}

TEST(P4_3_HashJoin, DISABLED_SameRowsAsNestedLoop) {
  MemStore store;
  store.Insert({10, 1, 100});
  store.Insert({10, 2, 200});
  store.Insert({20, 1, 100});
  Config config;
  IoStats stats;
  NullTraceSink trace;
  ExecutorContext ctx(&store, nullptr, &config, nullptr, nullptr, nullptr, nullptr, &stats, &trace,
                      nullptr);
  Schema schema{{{"s", 0}, {"o", 1}, {"x", 2}}};
  BoundTriplePattern left_pattern;
  left_pattern.subject = BoundVar(0);
  left_pattern.predicate = BoundId(1);
  left_pattern.object = BoundVar(1);
  BoundTriplePattern right_pattern;
  right_pattern.subject = BoundVar(0);
  right_pattern.predicate = BoundId(2);
  right_pattern.object = BoundVar(2);
  auto left = std::make_unique<TripleScanPlan>(schema, left_pattern, 3);
  auto right = std::make_unique<TripleScanPlan>(schema, right_pattern, 3);
  auto nlj_plan = std::make_unique<NestedLoopJoinPlan>(schema, nullptr, nullptr, 3);
  NestedLoopJoinExecutor nlj(std::make_unique<TripleScanExecutor>(&ctx, left.get()),
                             std::make_unique<TripleScanExecutor>(&ctx, right.get()),
                             nlj_plan.get());
  const auto expect = RowsOf(&nlj);
  auto hash_plan = std::make_unique<HashJoinPlan>(
      schema, std::make_unique<TripleScanPlan>(schema, left_pattern, 3),
      std::make_unique<TripleScanPlan>(schema, right_pattern, 3));
  auto hash = ExecutorFactory::Create(hash_plan.get(), &ctx);
  EXPECT_EQ(RowsOf(hash.get()), expect);
}

TEST(P4_4_DistinctLimit, DISABLED_DropsDuplicatesAndWindows) {
  MemStore store;
  store.Insert({1, 9, 5});
  store.Insert({2, 9, 5});
  store.Insert({3, 9, 3});
  store.Insert({4, 9, 1});
  store.Insert({5, 9, 2});
  Config config;
  IoStats stats;
  NullTraceSink trace;
  ExecutorContext ctx(&store, nullptr, &config, nullptr, nullptr, nullptr, nullptr, &stats, &trace,
                      nullptr);
  auto scan = ScanPlan(9, 2);
  Schema one{{{"o", 0}}};
  auto projection = std::make_unique<ProjectionPlan>(
      one, std::unique_ptr<AbstractPlanNode>(scan.release()), std::vector<size_t>{1});
  auto distinct =
      std::make_unique<DistinctPlan>(one, std::unique_ptr<AbstractPlanNode>(projection.release()));
  auto exec = ExecutorFactory::Create(distinct.get(), &ctx);
  const auto rows = RowsOf(exec.get());
  EXPECT_EQ(rows.size(), 4u);

  auto scan2 = ScanPlan(9, 2);
  auto limited = std::make_unique<LimitPlan>(
      scan2->OutputSchema(), std::unique_ptr<AbstractPlanNode>(scan2.release()), 2, 1);
  auto window = ExecutorFactory::Create(limited.get(), &ctx);
  window->Init();
  std::vector<term_id_t> objects;
  Row row;
  while (window->Next(&row)) {
    objects.push_back(row.at(1));
  }
  EXPECT_EQ(objects, (std::vector<term_id_t>{5, 3}));
}

TEST(P4_5_Sort, DISABLED_OrdersAscendingAndDescending) {
  MemStore store;
  store.Insert({1, 9, 3});
  store.Insert({1, 9, 1});
  store.Insert({1, 9, 2});
  Config config;
  IoStats stats;
  NullTraceSink trace;
  ExecutorContext ctx(&store, nullptr, &config, nullptr, nullptr, nullptr, nullptr, &stats, &trace,
                      nullptr);
  auto scan = ScanPlan(9, 2);
  auto sorted = std::make_unique<SortPlan>(scan->OutputSchema(),
                                           std::unique_ptr<AbstractPlanNode>(scan.release()),
                                           std::vector<BoundOrderKey>{{1, true, "o"}});
  auto exec = ExecutorFactory::Create(sorted.get(), &ctx);
  exec->Init();
  std::vector<term_id_t> objects;
  Row row;
  while (exec->Next(&row)) {
    objects.push_back(row.at(1));
  }
  EXPECT_EQ(objects, (std::vector<term_id_t>{1, 2, 3}));

  auto scan2 = ScanPlan(9, 2);
  auto desc = std::make_unique<SortPlan>(scan2->OutputSchema(),
                                         std::unique_ptr<AbstractPlanNode>(scan2.release()),
                                         std::vector<BoundOrderKey>{{1, false, "o"}});
  auto desc_exec = ExecutorFactory::Create(desc.get(), &ctx);
  desc_exec->Init();
  objects.clear();
  while (desc_exec->Next(&row)) {
    objects.push_back(row.at(1));
  }
  EXPECT_EQ(objects, (std::vector<term_id_t>{3, 2, 1}));
}

TEST(P4_6_InsertDelete, DISABLED_UpdatesTheStore) {
  MemStore store;
  Config config;
  IoStats stats;
  NullTraceSink trace;
  ExecutorContext ctx(&store, nullptr, &config, nullptr, nullptr, nullptr, nullptr, &stats, &trace,
                      nullptr);
  auto insert = std::make_unique<InsertPlan>(std::vector<Triple>{{1, 2, 3}, {1, 2, 3}, {4, 5, 6}});
  auto inserter = ExecutorFactory::Create(insert.get(), &ctx);
  inserter->Init();
  Row row;
  while (inserter->Next(&row)) {
  }
  auto iter = store.Scan({});
  iter->Init();
  std::vector<Triple> got;
  Triple triple;
  while (iter->Next(&triple)) {
    got.push_back(triple);
  }
  ASSERT_EQ(got.size(), 2u);
  auto del = std::make_unique<DeletePlan>(std::vector<Triple>{{1, 2, 3}});
  auto deleter = ExecutorFactory::Create(del.get(), &ctx);
  deleter->Init();
  while (deleter->Next(&row)) {
  }
  iter = store.Scan({});
  iter->Init();
  got.clear();
  while (iter->Next(&triple)) {
    got.push_back(triple);
  }
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0], (Triple{4, 5, 6}));
}

}  // namespace
}  // namespace ontodb
