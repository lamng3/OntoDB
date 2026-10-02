#pragma once

#include <memory>

#include "execution/executors/abstract_executor.h"
#include "execution/plans/projection_plan.h"

namespace ontodb {

class ProjectionExecutor : public AbstractExecutor {
 public:
  ProjectionExecutor(std::unique_ptr<AbstractExecutor> child, const ProjectionPlan* plan);

  void Init() override;
  auto Next(Row* row) -> bool override;
  auto GetOutputSchema() const -> const Schema& override;

 private:
  std::unique_ptr<AbstractExecutor> child_;
  const ProjectionPlan* plan_;
};

}  // namespace ontodb
