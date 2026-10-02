#pragma once

#include "binder/bound_query.h"
#include "execution/plans/abstract_plan.h"

namespace ontodb {

class IndexNestedLoopJoinPlan : public AbstractPlanNode {
 public:
  IndexNestedLoopJoinPlan(Schema output, PlanNodeRef left, PlanNodeRef right)
      : AbstractPlanNode(std::move(output), {}) {
    children_.push_back(std::move(left));
    children_.push_back(std::move(right));
  }
  auto GetType() const -> PlanType override { return PlanType::kIndexNestedLoopJoin; }
  auto Label() const -> std::string override { return "IndexNestedLoopJoin"; }
};

class HashJoinPlan : public AbstractPlanNode {
 public:
  HashJoinPlan(Schema output, PlanNodeRef left, PlanNodeRef right)
      : AbstractPlanNode(std::move(output), {}) {
    children_.push_back(std::move(left));
    children_.push_back(std::move(right));
  }
  auto GetType() const -> PlanType override { return PlanType::kHashJoin; }
  auto Label() const -> std::string override { return "HashJoin"; }
};

class DistinctPlan : public AbstractPlanNode {
 public:
  explicit DistinctPlan(Schema output, PlanNodeRef child)
      : AbstractPlanNode(std::move(output), {}) {
    children_.push_back(std::move(child));
  }
  auto GetType() const -> PlanType override { return PlanType::kDistinct; }
  auto Label() const -> std::string override { return "Distinct"; }
};

class LimitPlan : public AbstractPlanNode {
 public:
  LimitPlan(Schema output, PlanNodeRef child, std::optional<uint64_t> limit,
            std::optional<uint64_t> offset)
      : AbstractPlanNode(std::move(output), {}), limit_(limit), offset_(offset) {
    children_.push_back(std::move(child));
  }
  auto GetType() const -> PlanType override { return PlanType::kLimit; }
  auto Label() const -> std::string override {
    return "Limit " + std::to_string(limit_.value_or(0)) + " offset " +
           std::to_string(offset_.value_or(0));
  }
  auto Limit() const -> std::optional<uint64_t> { return limit_; }
  auto Offset() const -> std::optional<uint64_t> { return offset_; }

 private:
  std::optional<uint64_t> limit_;
  std::optional<uint64_t> offset_;
};

class SortPlan : public AbstractPlanNode {
 public:
  SortPlan(Schema output, PlanNodeRef child, std::vector<BoundOrderKey> keys)
      : AbstractPlanNode(std::move(output), {}), keys_(std::move(keys)) {
    children_.push_back(std::move(child));
  }
  auto GetType() const -> PlanType override { return PlanType::kSort; }
  auto Label() const -> std::string override {
    std::string text = "Sort [";
    for (size_t i = 0; i < keys_.size(); i++) {
      if (i != 0) {
        text += ", ";
      }
      text += "?" + keys_[i].name;
      text += keys_[i].ascending ? " ASC" : " DESC";
    }
    text += "]";
    return text;
  }
  auto Keys() const -> const std::vector<BoundOrderKey>& { return keys_; }

 private:
  std::vector<BoundOrderKey> keys_;
};

class InsertPlan : public AbstractPlanNode {
 public:
  explicit InsertPlan(std::vector<Triple> triples)
      : AbstractPlanNode(Schema{}, {}), triples_(std::move(triples)) {}
  auto GetType() const -> PlanType override { return PlanType::kInsert; }
  auto Label() const -> std::string override {
    return "Insert " + std::to_string(triples_.size()) + " triples";
  }
  auto Triples() const -> const std::vector<Triple>& { return triples_; }

 private:
  std::vector<Triple> triples_;
};

class DeletePlan : public AbstractPlanNode {
 public:
  explicit DeletePlan(std::vector<Triple> triples)
      : AbstractPlanNode(Schema{}, {}), triples_(std::move(triples)) {}
  auto GetType() const -> PlanType override { return PlanType::kDelete; }
  auto Label() const -> std::string override {
    return "Delete " + std::to_string(triples_.size()) + " triples";
  }
  auto Triples() const -> const std::vector<Triple>& { return triples_; }

 private:
  std::vector<Triple> triples_;
};

}  // namespace ontodb
