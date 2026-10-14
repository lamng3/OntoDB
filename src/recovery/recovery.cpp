#include "common/exception.h"
#include "recovery/checkpoint_manager.h"
#include "recovery/log_manager.h"
#include "recovery/log_record.h"
#include "recovery/log_recovery.h"

namespace ontodb {

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
