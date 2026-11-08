#pragma once

#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "recovery/checkpoint_manager.h"

namespace ontodb {

// A background pass over a page-store log. Every second it takes a fuzzy
// checkpoint (writers are not stopped), reclaims page ids whose removal is
// durable, and rewrites the log so it starts at the oldest record recovery
// can still need: the checkpoint, the dirty pages' recLSNs, and the records
// of transactions that were running at the checkpoint.
class LogCompactor {
 public:
  LogCompactor(LogManager* log, BufferPoolManager* bpm, TransactionManager* txns);
  ~LogCompactor();

  LogCompactor(const LogCompactor&) = delete;
  auto operator=(const LogCompactor&) -> LogCompactor& = delete;

  void Start();
  void Stop();
  // One pass, without the thread. Tests call this.
  void RunOnce();
  // The LSN the log must keep from, given the checkpoint record at
  // `checkpoint`: the smallest of its begin record (or the checkpoint itself
  // when it has none), every dirty page's recLSN, and the first record of
  // every transaction it lists as active.
  static auto OldestNeeded(const std::vector<LogRecord>& records, lsn_t checkpoint) -> lsn_t;

 private:
  void Loop();

  LogManager* log_;
  BufferPoolManager* bpm_;
  CheckpointManager checkpoints_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::thread thread_;
  bool stop_{false};
};

}  // namespace ontodb
