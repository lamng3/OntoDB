#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <sstream>

#include "common/exception.h"
#include "concurrency/transaction_manager.h"
#include "recovery/checkpoint_manager.h"
#include "recovery/log_recovery.h"
#include "store/indexed_store.h"

namespace ontodb {

namespace {

// Recovery with no indexes to undo into: the CLRs are still written.
class NoStore : public TripleStore {
 public:
  auto Insert(const Triple&) -> bool override { return false; }
  auto Delete(const Triple&) -> bool override { return false; }
  auto Scan(const TriplePattern&) -> std::unique_ptr<TripleIterator> override {
    throw std::logic_error("recovery has no store to scan");
  }
};

}  // namespace

CheckpointManager::CheckpointManager(LogManager* log, BufferPoolManager* bpm,
                                     TransactionManager* txn_mgr)
    : log_(log), bpm_(bpm), txn_mgr_(txn_mgr) {}

// A blocking ("sharp") checkpoint: write every dirty page (the pool forces
// the log first, by the write-ahead rule), then log the active transactions.
// No page change older than the checkpoint record can be missing from disk,
// so analysis starts its dirty page table there. Updates should not run
// while it does; that is the blocking part.
void CheckpointManager::Checkpoint() {
  bpm_->FlushAllPages();
  auto record = LogRecord::Checkpoint(txn_mgr_->ActiveTransactions());
  const lsn_t lsn = log_->Append(&record);
  log_->Flush(lsn, true);
  last_ = lsn;
}

auto CheckpointManager::LastCheckpointLsn() const -> lsn_t {
  return last_;
}

LogRecovery::LogRecovery(LogManager* log, BufferPoolManager* bpm, DiskManager* disk,
                         Catalog* catalog)
    : log_(log), bpm_(bpm), disk_(disk), catalog_(catalog) {}

void LogRecovery::Analyze() {
  records_ = log_->ReadAll();
  enum class Fate { kActive, kCommitted, kEnded };
  std::map<txn_id_t, Fate> fate;
  size_t start = 0;
  for (size_t i = 0; i < records_.size(); i++) {
    const LogRecord& r = records_[i];
    switch (r.GetType()) {
      case LogRecordType::kBegin:
      case LogRecordType::kUpdate:
      case LogRecordType::kClr:
        fate.emplace(r.GetTxnId(), Fate::kActive);
        break;
      case LogRecordType::kCommit:
        fate[r.GetTxnId()] = Fate::kCommitted;
        break;
      case LogRecordType::kAbort:
        fate[r.GetTxnId()] = Fate::kEnded;
        break;
      case LogRecordType::kCheckpoint:
        start = i;
        for (const txn_id_t txn : r.GetActiveTxns()) {
          fate.emplace(txn, Fate::kActive);
        }
        break;
      default:
        break;
    }
  }
  winners_.clear();
  losers_.clear();
  for (const auto& [txn, f] : fate) {
    if (f == Fate::kCommitted) {
      winners_.push_back(txn);
    } else if (f == Fate::kActive) {
      losers_.push_back(txn);
    }
  }
  dirty_.clear();
  for (size_t i = start; i < records_.size(); i++) {
    if (records_[i].GetType() == LogRecordType::kPage) {
      dirty_.emplace(records_[i].GetPageId(), records_[i].GetLsn());
    }
  }
  redo_lsn_ = INVALID_LSN;
  for (const auto& [page, rec_lsn] : dirty_) {
    redo_lsn_ = redo_lsn_ == INVALID_LSN ? rec_lsn : std::min(redo_lsn_, rec_lsn);
  }
  std::ostringstream out;
  out << "analysis: " << records_.size() << " records, " << winners_.size() << " winners, "
      << losers_.size() << " losers, " << dirty_.size() << " dirty pages, redo from lsn "
      << redo_lsn_;
  analysis_ = out.str();
}

void LogRecovery::Redo() {
  size_t applied = 0;
  size_t skipped = 0;
  for (const LogRecord& r : records_) {
    if (r.GetType() != LogRecordType::kPage || redo_lsn_ == INVALID_LSN || r.GetLsn() < redo_lsn_) {
      continue;
    }
    const auto entry = dirty_.find(r.GetPageId());
    if (entry == dirty_.end() || entry->second > r.GetLsn()) {
      skipped++;
      continue;
    }
    Page* page = bpm_->FetchPage(r.GetPageId());
    if (page == nullptr) {
      throw StorageException("recovery could not fetch page " + std::to_string(r.GetPageId()));
    }
    const bool stale = page->GetLSN() < r.GetLsn();
    if (stale) {
      std::memcpy(page->GetData() + kPageLsnSize, r.GetImage().data(), r.GetImage().size());
      page->SetLSN(r.GetLsn());
      applied++;
    } else {
      skipped++;
    }
    bpm_->UnpinPage(r.GetPageId(), stale);
  }
  redo_ = "redo: " + std::to_string(applied) + " page images applied, " + std::to_string(skipped) +
          " already on disk";
}

void LogRecovery::Undo() {
  std::unique_ptr<TripleStore> store;
  if (catalog_ != nullptr && catalog_->GetIndexRoot("spo") != INVALID_PAGE_ID) {
    store = std::make_unique<IndexedStore>(catalog_, bpm_, nullptr);
  } else {
    store = std::make_unique<NoStore>();
  }
  Undo(store.get());
}

void LogRecovery::Undo(TripleStore* store) {
  size_t undone = 0;
  for (const txn_id_t txn : losers_) {
    std::vector<LogRecord> chain;
    lsn_t last = INVALID_LSN;
    for (const LogRecord& r : records_) {
      if (r.GetTxnId() != txn) {
        continue;
      }
      last = r.GetLsn();
      if (r.GetType() == LogRecordType::kUpdate || r.GetType() == LogRecordType::kClr) {
        chain.push_back(r);
      }
    }
    undone += TransactionManager::UndoChain(txn, chain, store, log_, last);
    auto end = LogRecord::Abort(txn);
    log_->Append(&end);
  }
  if (log_->NextLsn() > 1) {
    log_->Flush(log_->NextLsn() - 1, true);
  }
  undo_ = "undo: " + std::to_string(undone) + " updates taken back in " +
          std::to_string(losers_.size()) + " losers";
}

auto LogRecovery::Summary() const -> std::string {
  return analysis_ + "\n" + redo_ + "\n" + undo_ + "\n";
}

auto LogRecovery::WinnerTxns() const -> std::vector<txn_id_t> {
  return winners_;
}

auto LogRecovery::LoserTxns() const -> std::vector<txn_id_t> {
  return losers_;
}

}  // namespace ontodb
