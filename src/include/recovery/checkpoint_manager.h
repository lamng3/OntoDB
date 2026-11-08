#pragma once

#include "buffer/buffer_pool_manager.h"
#include "concurrency/transaction_manager.h"
#include "recovery/log_manager.h"

namespace ontodb {

// Writes a checkpoint record and remembers its LSN. Checkpoint is sharp: it
// writes every dirty page first, so writers wait out the I/O. CheckpointFuzzy
// only records who is running and which pages are dirty, and returns while
// writers continue: it appends a begin record first, copies the tables, and
// then appends the checkpoint record, so anything logged during the copy lies
// after the begin record, where recovery and truncation read from. After
// either returns, LastCheckpointLsn is the LSN of the checkpoint record and
// it is durable.
class CheckpointManager {
 public:
  CheckpointManager(LogManager* log, BufferPoolManager* bpm, TransactionManager* txn_mgr);
  void Checkpoint();
  void CheckpointFuzzy();
  auto LastCheckpointLsn() const -> lsn_t;

 private:
  LogManager* log_;
  BufferPoolManager* bpm_;
  TransactionManager* txn_mgr_;
  lsn_t last_{INVALID_LSN};
};

}  // namespace ontodb
