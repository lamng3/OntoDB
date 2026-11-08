#pragma once

#include <condition_variable>
#include <mutex>
#include <thread>

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
