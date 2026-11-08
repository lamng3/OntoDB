#include "recovery/log_compactor.h"

#include <chrono>
#include <set>

namespace ontodb {

LogCompactor::LogCompactor(LogManager* log, BufferPoolManager* bpm, TransactionManager* txns)
    : log_(log), bpm_(bpm), checkpoints_(log, bpm, txns) {}

LogCompactor::~LogCompactor() {
  Stop();
}

void LogCompactor::Start() {
  std::lock_guard<std::mutex> guard(mu_);
  if (thread_.joinable()) {
    return;
  }
  stop_ = false;
  thread_ = std::thread([this] { Loop(); });
}

void LogCompactor::Stop() {
  {
    std::lock_guard<std::mutex> guard(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void LogCompactor::Loop() {
  std::unique_lock<std::mutex> lock(mu_);
  while (!stop_) {
    if (cv_.wait_for(lock, std::chrono::seconds(1), [this] { return stop_; })) {
      return;
    }
    lock.unlock();
    try {
      RunOnce();
    } catch (const std::exception&) {
      // The next pass tries again. A failed rewrite leaves the old log in place.
    }
    lock.lock();
  }
}

void LogCompactor::RunOnce() {
  checkpoints_.CheckpointFuzzy();
  bpm_->Reclaim();
  const lsn_t checkpoint = checkpoints_.LastCheckpointLsn();
  if (checkpoint == INVALID_LSN) {
    return;
  }
  const auto records = log_->ReadAll();
  std::set<txn_id_t> active;
  lsn_t keep = checkpoint;
  for (const auto& record : records) {
    if (record.GetLsn() != checkpoint || record.GetType() != LogRecordType::kCheckpoint) {
      continue;
    }
    for (const txn_id_t txn : record.GetActiveTxns()) {
      active.insert(txn);
    }
    for (const auto& entry : record.GetDirtyPages()) {
      const lsn_t rec_lsn = entry.second;
      if (rec_lsn <= 0) {
        keep = 1;  // dirtied, but the first record was not recorded: drop nothing
      } else if (rec_lsn < keep) {
        keep = rec_lsn;
      }
    }
  }
  for (const auto& record : records) {
    if (record.GetLsn() >= checkpoint || !active.contains(record.GetTxnId())) {
      continue;
    }
    keep = std::min(keep, record.GetLsn());
  }
  log_->Truncate(keep);
}

}  // namespace ontodb
