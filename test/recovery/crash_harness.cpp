#include "recovery/crash_harness.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "buffer/buffer_pool_manager.h"
#include "catalog/catalog.h"
#include "common/crash_point.h"
#include "common/types.h"
#include "concurrency/lock_manager.h"
#include "concurrency/transaction_manager.h"
#include "recovery/log_manager.h"
#include "recovery/log_recovery.h"
#include "storage/disk/disk_manager.h"
#include "store/indexed_store.h"
#include "test_util.h"

extern char** environ;

namespace ontodb::test {
namespace {

enum class Mode { kCorrect, kLoseCommit, kKeepUncommitted };

struct Op {
  enum class Type { kBegin, kInsert, kDelete, kCommit, kAbort } type;
  int txn{0};
  Triple triple;
};

std::string g_executable;

auto FormatTriple(const Triple& triple) -> std::string {
  return std::to_string(triple.subject) + " " + std::to_string(triple.predicate) + " " +
         std::to_string(triple.object);
}

auto ApplyBuffered(std::set<Triple>* data, const std::vector<std::pair<bool, Triple>>& ops) {
  for (const auto& op : ops) {
    if (op.first) {
      data->insert(op.second);
    } else {
      data->erase(op.second);
    }
  }
}

auto MakeWorkload(uint32_t seed) -> std::vector<Op> {
  std::mt19937 rng(seed);
  std::uniform_int_distribution<int> term(1, 12);
  std::uniform_int_distribution<int> coin(0, 99);
  std::vector<Op> ops;
  std::vector<Triple> inserted;
  for (int txn = 1; txn <= 10; txn++) {
    ops.push_back(Op{Op::Type::kBegin, txn, {}});
    const int n = 1 + static_cast<int>(rng() % 6);
    for (int i = 0; i < n; i++) {
      const bool insert = inserted.empty() || coin(rng) < 70;
      Triple triple{static_cast<term_id_t>(term(rng)), static_cast<term_id_t>(term(rng)),
                    static_cast<term_id_t>(term(rng))};
      if (insert) {
        inserted.push_back(triple);
      } else {  // mostly delete something that was inserted, so pages merge
        triple = inserted[rng() % inserted.size()];
      }
      ops.push_back(Op{insert ? Op::Type::kInsert : Op::Type::kDelete, txn, triple});
    }
    const bool commit = txn == 1 || coin(rng) < 70;
    ops.push_back(Op{commit ? Op::Type::kCommit : Op::Type::kAbort, txn, {}});
  }
  return ops;
}

// Which crash point reach kills the child: 0 for none (the parent kills it
// between operations), otherwise the n-th, counting every named crash point.
auto CrashReach(uint32_t seed) -> int {
  std::mt19937 rng(seed ^ 0x9e3779b9U);
  return seed % 3 == 0 ? 0 : 1 + static_cast<int>(rng() % 120);
}

auto Oracle(const std::vector<Op>& ops, size_t crash_after) -> std::set<Triple> {
  std::set<Triple> data;
  std::map<int, std::vector<std::pair<bool, Triple>>> buf;
  for (size_t i = 0; i < crash_after && i < ops.size(); i++) {
    const auto& op = ops[i];
    if (op.type == Op::Type::kBegin) {
      buf[op.txn].clear();
    } else if (op.type == Op::Type::kInsert) {
      buf[op.txn].push_back({true, op.triple});
    } else if (op.type == Op::Type::kDelete) {
      buf[op.txn].push_back({false, op.triple});
    } else if (op.type == Op::Type::kCommit) {
      ApplyBuffered(&data, buf[op.txn]);
      buf.erase(op.txn);
    } else {
      buf.erase(op.txn);
    }
  }
  return data;
}

class WalFakeStore {
 public:
  WalFakeStore(std::filesystem::path dir, Mode mode) : dir_(std::move(dir)), mode_(mode) {
    std::filesystem::create_directories(dir_);
    Load();
  }

  void Begin(int txn) { active_[txn].clear(); }

  void Insert(int txn, const Triple& triple) { Write(txn, true, triple); }

  void Delete(int txn, const Triple& triple) { Write(txn, false, triple); }

  void Commit(int txn) {
    if (mode_ == Mode::kLoseCommit) {
      ApplyBuffered(&memory_, active_[txn]);
      active_.erase(txn);
      return;
    }
    std::ofstream out(LogPath(), std::ios::app);
    for (const auto& op : active_[txn]) {
      out << txn << (op.first ? " I " : " D ") << FormatTriple(op.second) << '\n';
    }
    out << txn << " C\n";
    out.flush();
    out.close();
    Sync();
    ApplyBuffered(&memory_, active_[txn]);
    active_.erase(txn);
  }

  void Abort(int) {}

  void Crash() {
    memory_.clear();
    active_.clear();
    Load();
  }

  auto Contents() const -> std::vector<Triple> {
    return std::vector<Triple>(memory_.begin(), memory_.end());
  }

 private:
  auto LogPath() const -> std::filesystem::path { return dir_ / "wal.log"; }

  void Sync() const {
    const int fd = ::open(LogPath().c_str(), O_RDONLY);
    if (fd >= 0) {
      ::fsync(fd);
      ::close(fd);
    }
  }

  void Write(int txn, bool insert, const Triple& triple) {
    active_[txn].push_back({insert, triple});
    if (mode_ == Mode::kKeepUncommitted) {
      std::ofstream out(LogPath(), std::ios::app);
      out << txn << (insert ? " I " : " D ") << FormatTriple(triple) << '\n';
      out.flush();
      out.close();
      Sync();
      if (insert) {
        memory_.insert(triple);
      } else {
        memory_.erase(triple);
      }
    }
  }

  void Load() {
    memory_.clear();
    std::ifstream in(LogPath());
    std::map<int, std::vector<std::pair<bool, Triple>>> pending;
    std::set<int> committed;
    std::string line;
    while (std::getline(in, line)) {
      std::istringstream row(line);
      int txn = 0;
      std::string kind;
      row >> txn >> kind;
      if (kind == "C") {
        committed.insert(txn);
        continue;
      }
      Triple triple;
      row >> triple.subject >> triple.predicate >> triple.object;
      pending[txn].push_back({kind == "I", triple});
    }
    if (mode_ == Mode::kKeepUncommitted) {
      for (const auto& entry : pending) {
        ApplyBuffered(&memory_, entry.second);
      }
      return;
    }
    for (const int txn : committed) {
      ApplyBuffered(&memory_, pending[txn]);
    }
  }

  std::filesystem::path dir_;
  Mode mode_;
  std::set<Triple> memory_;
  std::map<int, std::vector<std::pair<bool, Triple>>> active_;
};

// The real store (PLAN 8.7): three B+ tree indexes with at most four keys a
// node, so a few dozen triples split and merge pages, on a 16-frame pool, so
// pages are stolen. Every page change is logged as an image, every triple
// change as an update record. Opening a directory that already has a log runs
// ARIES first: a torn final record is cut off, then analysis, redo, the
// catalog, and undo.
class RealStore {
 public:
  static constexpr int kNodeMax = 4;

  explicit RealStore(const std::filesystem::path& dir)
      : disk_(PrepareLog(dir) / "db"), log_((dir / "wal").string()), pool_(16, &disk_, 2, &log_) {
    log_.StartFlushThread();
    LogRecovery recovery(&log_, &pool_, &disk_, nullptr);
    recovery.Analyze();
    recovery.Redo();
    catalog_ = std::make_unique<Catalog>(&pool_);
    store_ = std::make_unique<IndexedStore>(catalog_.get(), &pool_, nullptr,
                                            IndexedStore::DefaultPermutations(), kNodeMax);
    recovery.Undo(store_.get());
    txns_ = std::make_unique<TransactionManager>(&locks_, store_.get(), &log_);
  }

  auto Log() -> LogManager* { return &log_; }

  void Begin(int txn) { open_[txn] = txns_->Begin(IsolationLevel::kReadCommitted); }

  void Insert(int txn, const Triple& triple) { Write(txn, true, triple); }

  void Delete(int txn, const Triple& triple) { Write(txn, false, triple); }

  void Commit(int txn) { txns_->Commit(open_.at(txn)); }

  void Abort(int txn) { txns_->Abort(open_.at(txn)); }

  auto Contents() -> std::vector<Triple> { return Collect({}); }

  // What a lost or half-applied page change would break even when the SPO
  // scan looks right: a tree's shape, a point lookup, and the other two
  // indexes holding the same triples.
  auto Problems() -> std::vector<std::string> {
    std::vector<std::string> problems;
    if (const auto broken = store_->CheckInvariants(); broken.has_value()) {
      problems.push_back("broken tree: " + *broken);
    }
    const auto all = Contents();
    const std::set<Triple> spo(all.begin(), all.end());
    for (const auto& triple : spo) {
      if (!store_->Contains(triple)) {
        problems.push_back("lookup misses " + FormatTriple(triple));
      }
    }
    std::set<Triple> pos;
    std::set<Triple> osp;
    for (term_id_t id = 1; id <= 12; id++) {
      for (const auto& t : Collect({std::nullopt, id, std::nullopt})) {
        pos.insert(t);
      }
      for (const auto& t : Collect({std::nullopt, std::nullopt, id})) {
        osp.insert(t);
      }
    }
    if (pos != spo) {
      problems.push_back("POS holds " + std::to_string(pos.size()) + " triples, SPO " +
                         std::to_string(spo.size()));
    }
    if (osp != spo) {
      problems.push_back("OSP holds " + std::to_string(osp.size()) + " triples, SPO " +
                         std::to_string(spo.size()));
    }
    return problems;
  }

 private:
  // A kill can tear the log's last record; appending after it would hide every
  // later record behind it, so the file is cut back to its valid prefix.
  static auto PrepareLog(const std::filesystem::path& dir) -> std::filesystem::path {
    std::filesystem::create_directories(dir);
    const auto wal = dir / "wal";
    if (std::filesystem::exists(wal)) {
      const auto scan = ScanLog(wal);
      if (scan.valid_bytes < scan.file_bytes) {
        std::filesystem::resize_file(wal, scan.valid_bytes);
      }
    }
    return dir;
  }

  auto Collect(const TriplePattern& pattern) -> std::vector<Triple> {
    std::vector<Triple> triples;
    auto scan = store_->Scan(pattern);
    scan->Init();
    Triple triple;
    while (scan->Next(&triple)) {
      triples.push_back(triple);
    }
    return triples;
  }

  void Write(int txn, bool insert, const Triple& triple) {
    Transaction* t = open_.at(txn);
    if (store_->Contains(triple) != insert) {  // log only a change that will happen
      txns_->LogUpdate(t, insert, triple);
      insert ? store_->Insert(triple) : store_->Delete(triple);
    }
  }

  DiskManager disk_;
  LogManager log_;
  BufferPoolManager pool_;
  std::unique_ptr<Catalog> catalog_;
  std::unique_ptr<IndexedStore> store_;
  LockManager locks_;
  std::unique_ptr<TransactionManager> txns_;
  std::map<int, Transaction*> open_;
};

template<typename Store>
void Apply(Store* store, const Op& op) {
  switch (op.type) {
    case Op::Type::kBegin:
      store->Begin(op.txn);
      break;
    case Op::Type::kInsert:
      store->Insert(op.txn, op.triple);
      break;
    case Op::Type::kDelete:
      store->Delete(op.txn, op.triple);
      break;
    case Op::Type::kCommit:
      store->Commit(op.txn);
      break;
    case Op::Type::kAbort:
      store->Abort(op.txn);
      break;
  }
}

auto Diff(const std::set<Triple>& expect, const std::vector<Triple>& got,
          std::vector<std::string> problems = {}) -> CrashReport {
  std::set<Triple> actual(got.begin(), got.end());
  for (const auto& triple : expect) {
    if (!actual.contains(triple)) {
      problems.push_back("missing " + FormatTriple(triple));
    }
  }
  for (const auto& triple : actual) {
    if (!expect.contains(triple)) {
      problems.push_back("unexpected " + FormatTriple(triple));
    }
  }
  CrashReport report;
  report.ok = problems.empty();
  for (size_t i = 0; i < problems.size(); i++) {
    if (i != 0) {
      report.message.push_back('\n');
    }
    report.message += problems[i];
  }
  return report;
}

auto FixedSchedule() -> std::vector<Op> {
  return {Op{Op::Type::kBegin, 1, {}}, Op{Op::Type::kInsert, 1, {1, 2, 3}},
          Op{Op::Type::kCommit, 1, {}}, Op{Op::Type::kBegin, 2, {}},
          Op{Op::Type::kInsert, 2, {4, 5, 6}}};
}

auto RunMode(Mode mode, const std::vector<Op>& ops, size_t crash_after) -> CrashReport {
  TempDir dir;
  {
    WalFakeStore store(dir.path(), mode);
    for (size_t i = 0; i < crash_after && i < ops.size(); i++) {
      Apply(&store, ops[i]);
    }
    store.Crash();
    return Diff(Oracle(ops, crash_after), store.Contents());
  }
}

}  // namespace

void SetExecutablePath(std::string path) {
  g_executable = std::move(path);
}

auto ExecutablePath() -> const std::string& {
  return g_executable;
}

auto RunLoseCommit() -> CrashReport {
  return RunMode(Mode::kLoseCommit, FixedSchedule(), 5);
}

auto RunKeepUncommitted() -> CrashReport {
  return RunMode(Mode::kKeepUncommitted, FixedSchedule(), 5);
}

auto RunCorrectInProcess() -> CrashReport {
  return RunMode(Mode::kCorrect, FixedSchedule(), 5);
}

auto CrashChildMain(int argc, char** argv) -> int {
  if (argc < 4) {
    return 2;
  }
  RealStore store(argv[2]);
  const auto seed = static_cast<uint32_t>(std::stoul(argv[3]));
  const auto ops = MakeWorkload(seed);
  // Die in the middle of an operation: inside a split or a merge, or between
  // a log flush and a page write. Half the time flush first whatever the log
  // may flush, so a half-finished change would reach the file if it could.
  const int reach = CrashReach(seed);
  int reached = 0;
  CrashPoint::SetProbe([&](std::string_view) {
    if (++reached == reach) {
      if (seed % 2 == 0) {
        store.Log()->FlushAvailable();
      }
      std::_Exit(9);
    }
  });
  std::string line;
  size_t index = 0;
  while (std::getline(std::cin, line)) {
    if (line != "go" || index >= ops.size()) {
      break;
    }
    Apply(&store, ops[index]);
    std::cout << "done " << index << '\n' << std::flush;
    index++;
  }
  return 0;
}

auto RunForked(uint32_t seed) -> CrashReport {
  signal(SIGPIPE, SIG_IGN);  // the child may die mid-operation, closing its pipe
  TempDir dir;
  const auto ops = MakeWorkload(seed);
  const size_t crash_after = std::max<size_t>(3, ops.size() * 2 / 3);
  int in_pipe[2];
  int out_pipe[2];
  if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
    return {false, "pipe failed"};
  }
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, in_pipe[0], STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&actions, in_pipe[1]);
  posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
  const std::string seed_text = std::to_string(seed);
  const std::string dir_text = dir.path().string();
  char* args[] = {const_cast<char*>(g_executable.c_str()), const_cast<char*>("--crash-child"),
                  const_cast<char*>(dir_text.c_str()), const_cast<char*>(seed_text.c_str()),
                  nullptr};
  pid_t pid = 0;
  const int rc = posix_spawn(&pid, g_executable.c_str(), &actions, nullptr, args, environ);
  posix_spawn_file_actions_destroy(&actions);
  close(in_pipe[0]);
  close(out_pipe[1]);
  if (rc != 0) {
    close(in_pipe[1]);
    close(out_pipe[0]);
    return {false, "posix_spawn failed"};
  }
  std::string child_out;
  char buffer[256];
  size_t finished = 0;
  while (finished < crash_after) {
    const char go[] = "go\n";
    if (write(in_pipe[1], go, sizeof(go) - 1) < 0) {
      break;
    }
    std::string line;
    while (line.empty() || line.back() != '\n') {
      const ssize_t n = read(out_pipe[0], buffer, sizeof(buffer));
      if (n <= 0) {
        break;
      }
      child_out.append(buffer, buffer + n);
      const auto pos = child_out.find('\n');
      if (pos != std::string::npos) {
        line = child_out.substr(0, pos);
        child_out.erase(0, pos + 1);
        break;
      }
    }
    if (line.rfind("done ", 0) != 0) {
      break;
    }
    finished++;
  }
  close(in_pipe[1]);
  kill(pid, SIGKILL);
  int status = 0;
  waitpid(pid, &status, 0);
  close(out_pipe[0]);
  try {
    RealStore store(dir.path());
    return Diff(Oracle(ops, finished), store.Contents(), store.Problems());
  } catch (const std::exception& ex) {
    return {false, std::string("recovery or the check threw: ") + ex.what()};
  }
}

}  // namespace ontodb::test
