#include "recovery/crash_harness.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "common/types.h"
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
  for (int txn = 1; txn <= 6; txn++) {
    ops.push_back(Op{Op::Type::kBegin, txn, {}});
    const int n = 1 + static_cast<int>(rng() % 3);
    for (int i = 0; i < n; i++) {
      Triple triple{static_cast<term_id_t>(term(rng)), static_cast<term_id_t>(term(rng)),
                    static_cast<term_id_t>(term(rng))};
      const bool insert = coin(rng) < 80;
      ops.push_back(Op{insert ? Op::Type::kInsert : Op::Type::kDelete, txn, triple});
    }
    const bool commit = txn == 1 || coin(rng) < 70;
    ops.push_back(Op{commit ? Op::Type::kCommit : Op::Type::kAbort, txn, {}});
  }
  return ops;
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

void Apply(WalFakeStore* store, const Op& op) {
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

auto Diff(const std::set<Triple>& expect, const std::vector<Triple>& got) -> CrashReport {
  std::set<Triple> actual(got.begin(), got.end());
  std::vector<std::string> problems;
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
  WalFakeStore store(argv[2], Mode::kCorrect);
  const auto ops = MakeWorkload(static_cast<uint32_t>(std::stoul(argv[3])));
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
  WalFakeStore store(dir.path(), Mode::kCorrect);
  return Diff(Oracle(ops, finished), store.Contents());
}

}  // namespace ontodb::test
