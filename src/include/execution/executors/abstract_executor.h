#pragma once

#include "common/types.h"

namespace ontodb {

class AbstractExecutor {
 public:
  virtual ~AbstractExecutor() = default;
  virtual void Init() = 0;
  // Restart with the outer row of a join. Slots that are bound in `bindings`
  // may be used to narrow the work; the rows produced must still be checked
  // against the outer row by the caller. By default the bindings are ignored.
  virtual void InitWith(const Row& bindings) {
    (void)bindings;
    Init();
  }
  virtual auto Next(Row* row) -> bool = 0;
  virtual auto GetOutputSchema() const -> const Schema& = 0;
};

}  // namespace ontodb
