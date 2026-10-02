#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/types.h"

namespace ontodb {

enum class PlanType {
  kTripleScan,
  kNestedLoopJoin,
  kFilter,
  kProjection,
  kIndexNestedLoopJoin,
  kHashJoin,
  kDistinct,
  kLimit,
  kSort,
  kInsert,
  kDelete
};

class AbstractPlanNode;
using PlanNodeRef = std::unique_ptr<AbstractPlanNode>;

class AbstractPlanNode {
 public:
  AbstractPlanNode(Schema output, std::vector<PlanNodeRef> children)
      : output_(std::move(output)), children_(std::move(children)) {}
  virtual ~AbstractPlanNode() = default;

  virtual auto GetType() const -> PlanType = 0;
  virtual auto Label() const -> std::string = 0;

  auto OutputSchema() const -> const Schema& { return output_; }
  auto GetChildAt(size_t index) const -> const AbstractPlanNode* {
    return children_.at(index).get();
  }
  auto GetChildren() const -> const std::vector<PlanNodeRef>& { return children_; }
  auto ToString() const -> std::string { return Pretty(0); }

 protected:
  auto Pretty(int indent) const -> std::string {
    std::string text(static_cast<size_t>(indent), ' ');
    text += Label();
    text.push_back('\n');
    for (const auto& child : children_) {
      text += child->Pretty(indent + 2);
    }
    return text;
  }

  Schema output_;
  std::vector<PlanNodeRef> children_;
};

}  // namespace ontodb
