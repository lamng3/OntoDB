#include "common/exception.h"
#include "recovery/checkpoint_manager.h"
#include "recovery/log_manager.h"
#include "recovery/log_record.h"
#include "recovery/log_recovery.h"

namespace ontodb {

auto LogRecord::Size() const -> uint32_t {
  throw NotImplementedException("PLAN 8.1: LogRecord::Size");
}
void LogRecord::SerializeTo(char*) const {
  throw NotImplementedException("PLAN 8.1: LogRecord::SerializeTo");
}
auto LogRecord::Deserialize(const char*, size_t) -> LogRecord {
  throw NotImplementedException("PLAN 8.1: LogRecord::Deserialize");
}

auto LogRecord::Begin(txn_id_t txn) -> LogRecord {
  LogRecord record;
  record.type_ = LogRecordType::kBegin;
  record.txn_id_ = txn;
  return record;
}
auto LogRecord::Commit(txn_id_t txn) -> LogRecord {
  LogRecord record;
  record.type_ = LogRecordType::kCommit;
  record.txn_id_ = txn;
  return record;
}
auto LogRecord::Abort(txn_id_t txn) -> LogRecord {
  LogRecord record;
  record.type_ = LogRecordType::kAbort;
  record.txn_id_ = txn;
  return record;
}
auto LogRecord::Update(txn_id_t txn, page_id_t page, bool is_insert,
                       const Triple& triple) -> LogRecord {
  LogRecord record;
  record.type_ = LogRecordType::kUpdate;
  record.txn_id_ = txn;
  record.page_id_ = page;
  record.is_insert_ = is_insert;
  record.triple_ = triple;
  return record;
}
auto LogRecord::Clr(txn_id_t txn, page_id_t page, bool is_insert, const Triple& triple,
                    lsn_t undo_next) -> LogRecord {
  LogRecord record;
  record.type_ = LogRecordType::kClr;
  record.txn_id_ = txn;
  record.page_id_ = page;
  record.is_insert_ = is_insert;
  record.triple_ = triple;
  record.undo_next_ = undo_next;
  return record;
}
auto LogRecord::Checkpoint(const std::vector<txn_id_t>& active) -> LogRecord {
  LogRecord record;
  record.type_ = LogRecordType::kCheckpoint;
  record.active_ = active;
  return record;
}

LogManager::LogManager(std::string log_path, IoStats* stats)
    : log_path_(std::move(log_path)), stats_(stats) {}
LogManager::~LogManager() = default;
void LogManager::StartFlushThread() {
  throw NotImplementedException("PLAN 8.2: LogManager::StartFlushThread");
}
void LogManager::StopFlushThread() {
  throw NotImplementedException("PLAN 8.2: LogManager::StopFlushThread");
}
auto LogManager::Append(LogRecord*) -> lsn_t {
  throw NotImplementedException("PLAN 8.2: LogManager::Append");
}
void LogManager::Flush(lsn_t, bool) {
  throw NotImplementedException("PLAN 8.2: LogManager::Flush");
}
auto LogManager::GetPersistentLsn() const -> lsn_t {
  throw NotImplementedException("PLAN 8.2: LogManager::GetPersistentLsn");
}
auto LogManager::GetFlushCount() const -> uint64_t {
  throw NotImplementedException("PLAN 8.2: LogManager::GetFlushCount");
}
auto LogManager::DebugTail(size_t) const -> std::string {
  throw NotImplementedException("PLAN 8.2: LogManager::DebugTail");
}

CheckpointManager::CheckpointManager(LogManager* log, BufferPoolManager* bpm,
                                     TransactionManager* txn_mgr)
    : log_(log), bpm_(bpm), txn_mgr_(txn_mgr) {}
void CheckpointManager::Checkpoint() {
  throw NotImplementedException("PLAN 8.5: CheckpointManager::Checkpoint");
}
auto CheckpointManager::LastCheckpointLsn() const -> lsn_t {
  return last_;
}

LogRecovery::LogRecovery(LogManager* log, BufferPoolManager* bpm, DiskManager* disk,
                         Catalog* catalog)
    : log_(log), bpm_(bpm), disk_(disk), catalog_(catalog) {}
void LogRecovery::Analyze() {
  throw NotImplementedException("PLAN 8.6: LogRecovery::Analyze");
}
void LogRecovery::Redo() {
  throw NotImplementedException("PLAN 8.6: LogRecovery::Redo");
}
void LogRecovery::Undo() {
  throw NotImplementedException("PLAN 8.6: LogRecovery::Undo");
}
auto LogRecovery::Summary() const -> std::string {
  throw NotImplementedException("PLAN 8.6: LogRecovery::Summary");
}
auto LogRecovery::WinnerTxns() const -> std::vector<txn_id_t> {
  throw NotImplementedException("PLAN 8.6: LogRecovery::WinnerTxns");
}
auto LogRecovery::LoserTxns() const -> std::vector<txn_id_t> {
  throw NotImplementedException("PLAN 8.6: LogRecovery::LoserTxns");
}

}  // namespace ontodb
