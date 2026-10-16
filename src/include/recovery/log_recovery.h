#pragma once

#include <map>
#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "recovery/log_manager.h"
#include "recovery/log_record.h"
#include "storage/disk/disk_manager.h"

namespace ontodb {

// ARIES. Analyze finds the winner and loser sets and the redo LSN. Redo
// repeats every page update whose LSN is greater than the pageLSN, including
// CLRs. Undo walks losers newest-first and writes a CLR for each logical
// triple undo. A B+ tree split or merge is not undone as a logical triple
// operation; those pages are restored by redo only.
// Summary is one line per pass: "analysis ...", "redo ...", "undo ...".
//
// Analysis scans the log: a transaction with a commit record is a winner; one
// with updates or a begin but neither a commit nor an abort record (written
// when an abort finishes) is a loser. The dirty page table holds each page's
// first page record since the last checkpoint (checkpoints are sharp, so
// nothing older needs redo), and the redo LSN is the smallest of those.
// Redo copies each page record's image onto its page when the record's LSN
// is greater than the pageLSN, through the pool's raw interface so redo is
// not logged again; a page past the end of the file reads as zeros. Undo runs
// TransactionManager::UndoChain for each loser against the indexed store the
// catalog names (nothing to undo if it has none), so its page changes and
// CLRs are logged and a crash during undo is safe, then writes an abort
// record per loser.
class LogRecovery {
 public:
  LogRecovery(LogManager* log, BufferPoolManager* bpm, DiskManager* disk, Catalog* catalog);
  // Reopening an existing database: open the Catalog after Redo (redo may be
  // what restores its page) and hand it over here before Undo.
  void SetCatalog(Catalog* catalog) { catalog_ = catalog; }
  void Analyze();
  void Redo();
  void Undo();
  auto Summary() const -> std::string;
  auto WinnerTxns() const -> std::vector<txn_id_t>;
  auto LoserTxns() const -> std::vector<txn_id_t>;

 private:
  LogManager* log_;
  std::vector<LogRecord> records_;
  std::vector<txn_id_t> winners_;
  std::vector<txn_id_t> losers_;
  std::map<page_id_t, lsn_t> dirty_;  // page -> recLSN
  lsn_t redo_lsn_{INVALID_LSN};
  std::string analysis_;
  std::string redo_;
  std::string undo_;
  BufferPoolManager* bpm_;
  DiskManager* disk_;
  Catalog* catalog_;
};

}  // namespace ontodb
