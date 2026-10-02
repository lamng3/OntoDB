#pragma once

#include "buffer/buffer_pool_manager.h"
#include "concurrency/transaction_manager.h"
#include "recovery/log_manager.h"

namespace ontodb {

// Writes a checkpoint record and remembers its LSN. Blocking or fuzzy is your
// choice; say which in the comment above Checkpoint when you implement it.
// After Checkpoint returns, LastCheckpointLsn is the LSN of that record and
// the record is durable.
class CheckpointManager {
 public:
  CheckpointManager(LogManager* log, BufferPoolManager* bpm, TransactionManager* txn_mgr);
  void Checkpoint();
  auto LastCheckpointLsn() const -> lsn_t;

 private:
  LogManager* log_;
  BufferPoolManager* bpm_;
  TransactionManager* txn_mgr_;
  lsn_t last_{INVALID_LSN};
};

}  // namespace ontodb
