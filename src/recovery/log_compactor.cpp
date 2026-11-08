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
    } catch (const std::exception& ex) {
      // The next pass tries again. A failed rewrite leaves the old log in place.
      static_cast<void>(ex.what());
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
  log_->Truncate(OldestNeeded(log_->ReadAll(), checkpoint));
}

auto LogCompactor::OldestNeeded(const std::vector<LogRecord>& records, lsn_t checkpoint) -> lsn_t {
  std::set<txn_id_t> active;
  lsn_t keep = checkpoint;
  lsn_t begun = INVALID_LSN;  // the latest begin record before the checkpoint
  for (const auto& record : records) {
    if (record.GetLsn() >= checkpoint) {
      break;
    }
    if (record.GetType() == LogRecordType::kCheckpointBegin) {
      begun = record.GetLsn();
    } else if (record.GetType() == LogRecordType::kCheckpoint) {
      begun = INVALID_LSN;
    }
  }
  if (begun != INVALID_LSN) {
    keep = begun;
  }
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
  return keep;
}

}  // namespace ontodb
