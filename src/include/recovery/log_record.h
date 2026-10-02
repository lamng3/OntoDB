#pragma once

#include <cstdint>
#include <vector>

#include "common/types.h"

namespace ontodb {

// WAL record. The byte layout is yours (PLAN 8.1). SerializeTo writes Size()
// bytes. Deserialize reads one record and throws StorageException if `len`
// is shorter than a record. Update records carry the triple and whether the
// update inserted or deleted it. CLR records carry undo_next, the LSN of the
// next record to undo. A checkpoint record lists the active transactions.
enum class LogRecordType { kInvalid, kBegin, kCommit, kAbort, kUpdate, kClr, kCheckpoint };

class LogRecord {
 public:
  LogRecord() = default;

  auto GetType() const -> LogRecordType { return type_; }
  auto GetLsn() const -> lsn_t { return lsn_; }
  auto GetTxnId() const -> txn_id_t { return txn_id_; }
  auto GetPrevLsn() const -> lsn_t { return prev_lsn_; }
  auto GetPageId() const -> page_id_t { return page_id_; }
  auto GetUndoNext() const -> lsn_t { return undo_next_; }
  auto GetTriple() const -> Triple { return triple_; }
  auto IsInsert() const -> bool { return is_insert_; }
  auto GetActiveTxns() const -> const std::vector<txn_id_t>& { return active_; }

  void SetLsn(lsn_t lsn) { lsn_ = lsn; }
  void SetPrevLsn(lsn_t lsn) { prev_lsn_ = lsn; }

  auto Size() const -> uint32_t;
  void SerializeTo(char* dest) const;
  static auto Deserialize(const char* src, size_t len) -> LogRecord;

  static auto Begin(txn_id_t txn) -> LogRecord;
  static auto Commit(txn_id_t txn) -> LogRecord;
  static auto Abort(txn_id_t txn) -> LogRecord;
  static auto Update(txn_id_t txn, page_id_t page, bool is_insert,
                     const Triple& triple) -> LogRecord;
  static auto Clr(txn_id_t txn, page_id_t page, bool is_insert, const Triple& triple,
                  lsn_t undo_next) -> LogRecord;
  static auto Checkpoint(const std::vector<txn_id_t>& active) -> LogRecord;

 private:
  LogRecordType type_{LogRecordType::kInvalid};
  lsn_t lsn_{INVALID_LSN};
  txn_id_t txn_id_{INVALID_TXN_ID};
  lsn_t prev_lsn_{INVALID_LSN};
  page_id_t page_id_{INVALID_PAGE_ID};
  lsn_t undo_next_{INVALID_LSN};
  Triple triple_{};
  bool is_insert_{false};
  std::vector<txn_id_t> active_;
};

}  // namespace ontodb
