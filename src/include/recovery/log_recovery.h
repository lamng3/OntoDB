#pragma once

#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "recovery/log_manager.h"
#include "storage/disk/disk_manager.h"

namespace ontodb {

// ARIES. Analyze finds the winner and loser sets and the redo LSN. Redo
// repeats every page update whose LSN is greater than the pageLSN, including
// CLRs. Undo walks losers newest-first and writes a CLR for each logical
// triple undo. A B+ tree split or merge is not undone as a logical triple
// operation; those pages are restored by redo only.
// Summary is one line per pass: "analysis ...", "redo ...", "undo ...".
class LogRecovery {
 public:
  LogRecovery(LogManager* log, BufferPoolManager* bpm, DiskManager* disk, Catalog* catalog);
  void Analyze();
  void Redo();
  void Undo();
  auto Summary() const -> std::string;
  auto WinnerTxns() const -> std::vector<txn_id_t>;
  auto LoserTxns() const -> std::vector<txn_id_t>;

 private:
  LogManager* log_;
  BufferPoolManager* bpm_;
  DiskManager* disk_;
  Catalog* catalog_;
};

}  // namespace ontodb
