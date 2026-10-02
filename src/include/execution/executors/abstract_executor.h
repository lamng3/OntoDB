#pragma once

#include "common/types.h"

namespace ontodb {

class AbstractExecutor {
 public:
  virtual ~AbstractExecutor() = default;
  virtual void Init() = 0;
  virtual auto Next(Row* row) -> bool = 0;
  virtual auto GetOutputSchema() const -> const Schema& = 0;
};

}  // namespace ontodb
