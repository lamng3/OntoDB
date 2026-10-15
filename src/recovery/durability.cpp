#include "recovery/durability.h"

#include <algorithm>
#include <memory>
#include <set>
#include <sstream>
#include <vector>

#include "common/crash_point.h"
#include "common/exception.h"
#include "recovery/snapshot.h"
#include "store/recording_store.h"

namespace ontodb {
namespace {

struct ReplayResult {
  size_t records{0};
  size_t committed{0};
  size_t uncommitted{0};
  lsn_t last_lsn{0};
  txn_id_t max_txn{0};
};

// Loads a snapshot into an empty dictionary and store.
void LoadSnapshot(const Snapshot& snap, Dictionary& dictionary, TripleStore& store) {
  for (size_t i = 0; i < snap.terms.size(); i++) {
    if (dictionary.Insert(snap.terms[i]) != i + 1) {
      throw SnapshotCorruptionException("snapshot: term " + std::to_string(i + 1) +
                                        " is a duplicate spelling");
    }
  }
  for (const auto& triple : snap.triples) {
    store.Insert(triple);
  }
}

// Applies the records logged after `after_lsn`. Terms apply in order whatever
// their transaction's outcome: the dictionary only grows, and a term no
// committed triple uses is invisible. Updates apply only for committed
// transactions, in log order.
auto Replay(const std::vector<LogRecord>& records, lsn_t after_lsn, Dictionary& dictionary,
            TripleStore& store) -> ReplayResult {
  ReplayResult result;
  std::set<txn_id_t> begun;
  std::set<txn_id_t> committed;
  for (const auto& record : records) {
    result.max_txn = std::max(result.max_txn, record.GetTxnId());
    result.last_lsn = std::max(result.last_lsn, record.GetLsn());
    if (record.GetLsn() <= after_lsn) {
      continue;
    }
    result.records++;
    if (record.GetType() == LogRecordType::kBegin) {
      begun.insert(record.GetTxnId());
    } else if (record.GetType() == LogRecordType::kCommit) {
      committed.insert(record.GetTxnId());
    }
  }
  for (const auto& record : records) {
    if (record.GetLsn() <= after_lsn) {
      continue;
    }
    if (record.GetType() == LogRecordType::kTerm) {
      const term_id_t id = record.GetTermId();
      if (id <= dictionary.Size()) {
        if (dictionary.Lookup(id) != record.GetTermText()) {
          throw LogCorruptionException("wal: lsn " + std::to_string(record.GetLsn()) +
                                       " gives term " + std::to_string(id) +
                                       " a different spelling");
        }
      } else if (id != dictionary.Size() + 1 || dictionary.Insert(record.GetTermText()) != id) {
        throw LogCorruptionException("wal: lsn " + std::to_string(record.GetLsn()) +
                                     " skips to term " + std::to_string(id));
      }
    } else if (record.GetType() == LogRecordType::kUpdate &&
               committed.contains(record.GetTxnId())) {
      if (record.IsInsert()) {
        store.Insert(record.GetTriple());
      } else {
        store.Delete(record.GetTriple());
      }
    }
  }
  result.committed = committed.size();
  for (const txn_id_t txn : begun) {
    result.uncommitted += committed.contains(txn) ? 0 : 1;
  }
  return result;
}

auto AllTriples(TripleStore& store) -> std::vector<Triple> {
  std::vector<Triple> triples;
  auto iter = store.Scan({});
  iter->Init();
  Triple triple;
  while (iter->Next(&triple)) {
    triples.push_back(triple);
  }
  std::sort(triples.begin(), triples.end());
  return triples;
}

// What to do when the log ends in a damaged record (PLAN 9.5). One class per
// RecoveryMode, chosen once by MakeDamagePolicy.
class LogDamagePolicy {
 public:
  virtual ~LogDamagePolicy() = default;
  virtual void Handle(const LogScan& scan, const std::filesystem::path& wal,
                      OpenReport* report) const = 0;
};

// Keep the valid prefix, cut the log there, and report what was dropped.
class PointInTimePolicy : public LogDamagePolicy {
 public:
  void Handle(const LogScan& scan, const std::filesystem::path& wal,
              OpenReport* report) const override {
    report->stop = scan.stop;
    report->discarded_bytes = scan.file_bytes - scan.valid_bytes;
    std::filesystem::resize_file(wal, scan.valid_bytes);
  }
};

// Refuse to open.
class AbsoluteConsistencyPolicy : public LogDamagePolicy {
 public:
  void Handle(const LogScan& scan, const std::filesystem::path&, OpenReport*) const override {
    throw LogCorruptionException("wal: " + scan.stop +
                                 "; not opening in absolute-consistency mode");
  }
};

auto MakeDamagePolicy(RecoveryMode mode) -> std::unique_ptr<LogDamagePolicy> {
  switch (mode) {
    case RecoveryMode::kAbsoluteConsistency:
      return std::make_unique<AbsoluteConsistencyPolicy>();
    case RecoveryMode::kPointInTime:
      break;
  }
  return std::make_unique<PointInTimePolicy>();
}

}  // namespace

auto OpenReport::ToString() const -> std::string {
  std::ostringstream out;
  out << "opened " << dir.string() << ": ";
  if (had_snapshot) {
    out << "snapshot at lsn " << snapshot_lsn << " (" << snapshot_triples << " triples)";
  } else {
    out << "no snapshot";
  }
  out << "; wal " << log_records << (log_records == 1 ? " record" : " records") << ", " << committed
      << " committed, " << uncommitted << " uncommitted dropped; " << triples
      << (triples == 1 ? " triple" : " triples");
  if (!stop.empty()) {
    out << "\nwarning: wal stopped: " << stop << "; discarded " << discarded_bytes
        << " bytes after the last valid record";
  }
  return out.str();
}

Durability::Durability(std::filesystem::path dir, MemDictionary* dictionary, MemStore* store,
                       IoStats* stats)
    : dir_(std::move(dir)), dictionary_(dictionary), store_(store), stats_(stats) {}

Durability::~Durability() = default;

void Durability::StartLog(lsn_t next_lsn) {
  log_ = std::make_unique<LogManager>(WalPath().string(), stats_);
  log_->AdvanceNextLsn(next_lsn);
  log_->StartFlushThread();
}

auto Durability::Open(RecoveryMode mode) -> OpenReport {
  std::lock_guard<std::mutex> guard(mu_);
  std::filesystem::create_directories(dir_);
  OpenReport report;
  report.dir = dir_;
  const lsn_t snapshot_lsn = LoadSnapshotIfAny(&report);
  LogScan scan = ScanLog(WalPath());
  if (!scan.Clean()) {
    MakeDamagePolicy(mode)->Handle(scan, WalPath(), &report);
  }
  const lsn_t last_lsn = ReplayLog(scan, snapshot_lsn, &report);
  StartLog(std::max(snapshot_lsn, last_lsn) + 1);
  return report;
}

auto Durability::LoadSnapshotIfAny(OpenReport* report) -> lsn_t {
  if (!std::filesystem::exists(SnapshotPath())) {
    return 0;
  }
  const Snapshot snap = ReadSnapshot(SnapshotPath());
  LoadSnapshot(snap, *dictionary_, *store_);
  report->had_snapshot = true;
  report->snapshot_lsn = snap.lsn;
  report->snapshot_triples = snap.triples.size();
  next_txn_ = snap.next_txn;
  return snap.lsn;
}

auto Durability::ReplayLog(const LogScan& scan, lsn_t snapshot_lsn, OpenReport* report) -> lsn_t {
  const ReplayResult replay = Replay(scan.records, snapshot_lsn, *dictionary_, *store_);
  report->log_records = replay.records;
  report->committed = replay.committed;
  report->uncommitted = replay.uncommitted;
  report->triples = store_->Size();
  next_txn_ = std::max(next_txn_, replay.max_txn + 1);
  logged_terms_ = dictionary_->Size();
  return replay.last_lsn;
}

void Durability::Run(const std::function<void(TripleStore&)>& body) {
  std::lock_guard<std::mutex> guard(mu_);
  RecordingStore recording(store_);
  try {
    body(recording);
    LogTransaction(recording.Changes());
  } catch (...) {
    recording.Undo();
    throw;
  }
}

void Durability::LogTransaction(const std::vector<RecordingStore::Change>& changes) {
  const size_t terms = dictionary_->Size();
  if (changes.empty() && terms == logged_terms_) {
    return;  // nothing to make durable
  }
  const txn_id_t txn = next_txn_++;
  lsn_t prev = INVALID_LSN;
  auto append = [&](LogRecord record) {
    record.SetPrevLsn(prev);
    prev = log_->Append(&record);
  };
  append(LogRecord::Begin(txn));
  for (size_t id = logged_terms_ + 1; id <= terms; id++) {
    const auto text = dictionary_->Lookup(id);
    if (!text) {
      throw StorageException("dictionary has no term " + std::to_string(id));
    }
    append(LogRecord::Term(txn, id, *text));
  }
  for (const auto& change : changes) {
    append(LogRecord::Update(txn, INVALID_PAGE_ID, change.is_insert, change.triple));
  }
  CrashPoint::Reach("Durability::Run::before_commit");
  append(LogRecord::Commit(txn));
  log_->Flush(prev, true);
  logged_terms_ = terms;
}

auto Durability::Checkpoint() -> std::string {
  std::lock_guard<std::mutex> guard(mu_);
  const lsn_t lsn = log_->NextLsn() - 1;
  log_->Flush(lsn, true);
  WriteSnapshot(SnapshotPath(), *dictionary_, *store_, lsn, next_txn_);
  logged_terms_ = dictionary_->Size();
  // A crash here leaves the new snapshot and the old log. Replay skips every
  // record at or below the snapshot's LSN, so the old log does no harm.
  CrashPoint::Reach("Durability::Checkpoint::after_snapshot");
  log_.reset();
  std::filesystem::resize_file(WalPath(), 0);
  StartLog(lsn + 1);
  std::ostringstream out;
  out << "checkpoint at lsn " << lsn << ": " << dictionary_->Size() << " terms, " << store_->Size()
      << " triples; wal reset";
  return out.str();
}

auto Durability::Verify() -> std::string {
  std::lock_guard<std::mutex> guard(mu_);
  log_->Flush(log_->NextLsn() - 1, true);
  MemDictionary rebuilt_terms;
  MemStore rebuilt;
  lsn_t snapshot_lsn = 0;
  const LogScan scan = ScanLog(WalPath());
  const std::vector<CheckResult> checks = {
      CheckSnapshot(&rebuilt_terms, &rebuilt, &snapshot_lsn),
      CheckLog(scan),
      CheckReplay(scan, snapshot_lsn, &rebuilt_terms, &rebuilt),
      CheckInvariants(),
  };
  std::ostringstream out;
  bool ok = true;
  for (const auto& check : checks) {
    out << check.line << '\n';
    ok = ok && check.ok;
  }
  out << (ok ? "verify: ok" : "verify: FAILED");
  return out.str();
}

auto Durability::CheckSnapshot(MemDictionary* terms, MemStore* store,
                               lsn_t* snapshot_lsn) const -> CheckResult {
  if (!std::filesystem::exists(SnapshotPath())) {
    return {true, "snapshot: none"};
  }
  try {
    const Snapshot snap = ReadSnapshot(SnapshotPath());
    LoadSnapshot(snap, *terms, *store);
    *snapshot_lsn = snap.lsn;
    std::ostringstream line;
    line << "snapshot: ok (lsn " << snap.lsn << ", " << snap.terms.size() << " terms, "
         << snap.triples.size() << " triples)";
    return {true, line.str()};
  } catch (const StorageException& ex) {
    return {false, std::string("snapshot: FAILED: ") + ex.what()};
  }
}

auto Durability::CheckLog(const LogScan& scan) -> CheckResult {
  if (!scan.Clean()) {
    return {false, "wal: FAILED: " + scan.stop};
  }
  std::ostringstream line;
  line << "wal: ok (" << scan.records.size() << " records, " << scan.file_bytes << " bytes)";
  return {true, line.str()};
}

auto Durability::CheckReplay(const LogScan& scan, lsn_t snapshot_lsn, MemDictionary* terms,
                             MemStore* store) const -> CheckResult {
  try {
    Replay(scan.records, snapshot_lsn, *terms, *store);
  } catch (const StorageException& ex) {
    return {false, std::string("replay: FAILED: ") + ex.what()};
  }
  bool terms_match = terms->Size() <= dictionary_->Size();
  for (term_id_t id = 1; terms_match && id <= terms->Size(); id++) {
    terms_match = terms->Lookup(id) == dictionary_->Lookup(id);
  }
  const bool triples_match = AllTriples(*store) == AllTriples(*store_);
  if (terms_match && triples_match) {
    return {true,
            "replay: matches the live database (" + std::to_string(store->Size()) + " triples)"};
  }
  return {false, std::string("replay: MISMATCH (") + (terms_match ? "" : "terms ") +
                     (triples_match ? "" : "triples ") + "differ from the live database)"};
}

auto Durability::CheckInvariants() const -> CheckResult {
  const auto live = AllTriples(*store_);
  const size_t n = dictionary_->Size();
  const bool unique = std::adjacent_find(live.begin(), live.end()) == live.end();
  const bool resolved = std::all_of(live.begin(), live.end(), [n](const Triple& t) {
    return t.subject >= 1 && t.subject <= n && t.predicate >= 1 && t.predicate <= n &&
           t.object >= 1 && t.object <= n;
  });
  if (unique && resolved) {
    return {true, "invariants: ok (no duplicate triples, every id in the dictionary)"};
  }
  return {false, std::string("invariants: FAILED (") + (unique ? "" : "duplicate triples ") +
                     (resolved ? "" : "unresolved ids") + ")"};
}

auto Durability::LogTail(size_t n) const -> std::string {
  return log_->DebugTail(n);
}

}  // namespace ontodb
