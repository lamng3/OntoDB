#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/exception.h"
#include "common/types.h"

namespace ontodb {

// WAL record (PLAN 8.1). SerializeTo writes Size() bytes. Deserialize reads
// one record and throws StorageException if `len` is shorter than a record,
// and LogCorruptionException if the checksum or the type is wrong. Update
// records carry the triple and whether the update inserted or deleted it.
// CLR records carry undo_next, the LSN of the next record to undo. A
// checkpoint record lists the active transactions. A term record carries one
// dictionary entry, so a log replayed into an empty dictionary rebuilds the
// same ids (PLAN 9.3).
//
// Layout, native byte order:
//   u32 size        whole record, header included
//   u32 crc32c      over every byte after this field
//   u8  type
//   i64 lsn, i64 txn_id, i64 prev_lsn
//   then by type:
//     update      i32 page_id, u8 is_insert, u64 s, u64 p, u64 o
//     clr         update fields, then i64 undo_next
//     checkpoint  u32 n, then n x i64 txn_id
//     term        u64 term_id, u32 n, then n bytes of canonical spelling
//     page        i32 page_id, then the page's PAGE_SIZE - 8 bytes after its pageLSN
//     begin, commit, abort: nothing
//
// A page record (PLAN 8.3) is a physical after-image: redo copies it back
// when its LSN is newer than the page's pageLSN. Its txn_id is
// INVALID_TXN_ID; page records are redo-only and belong to no undo chain.
enum class LogRecordType {
  kInvalid,
  kBegin,
  kCommit,
  kAbort,
  kUpdate,
  kClr,
  kCheckpoint,
  kTerm,
  kPage
};

// A record whose bytes are all present but wrong: bad checksum, bad type, or
// an impossible size. A short buffer is a plain StorageException instead,
// because a torn final write is the expected shape of a crash.
class LogCorruptionException : public StorageException {
 public:
  using StorageException::StorageException;
};

class LogRecord {
 public:
  static constexpr uint32_t kHeaderSize = 4 + 4 + 1 + 8 + 8 + 8;
  static constexpr uint32_t kMaxSize = 1u << 24;

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
  auto GetTermId() const -> term_id_t { return term_id_; }
  auto GetTermText() const -> const std::string& { return term_text_; }
  auto GetImage() const -> const std::string& { return image_; }  // a page record's bytes

  void SetLsn(lsn_t lsn) { lsn_ = lsn; }
  void SetPrevLsn(lsn_t lsn) { prev_lsn_ = lsn; }

  auto Size() const -> uint32_t;
  void SerializeTo(char* dest) const;
  static auto Deserialize(const char* src, size_t len) -> LogRecord;
  auto ToString() const -> std::string;

  static auto Begin(txn_id_t txn) -> LogRecord;
  static auto Commit(txn_id_t txn) -> LogRecord;
  static auto Abort(txn_id_t txn) -> LogRecord;
  static auto Update(txn_id_t txn, page_id_t page, bool is_insert,
                     const Triple& triple) -> LogRecord;
  static auto Clr(txn_id_t txn, page_id_t page, bool is_insert, const Triple& triple,
                  lsn_t undo_next) -> LogRecord;
  static auto Checkpoint(const std::vector<txn_id_t>& active) -> LogRecord;
  static auto Term(txn_id_t txn, term_id_t id, const std::string& text) -> LogRecord;
  // `bytes` is the page after its pageLSN: PAGE_SIZE - 8 bytes.
  static auto Page(page_id_t page, const char* bytes) -> LogRecord;

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
  term_id_t term_id_{INVALID_TERM_ID};
  std::string term_text_;
  std::string image_;
};

}  // namespace ontodb
