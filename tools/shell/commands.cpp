#include "shell/commands.h"

#include <iomanip>
#include <sstream>

#include "common/exception.h"
#include "shell/text.h"

namespace ontodb {
namespace {

// Records \log prints when no count is given.
constexpr size_t kDefaultLogTail = 20;
// Width of the usage column in \help.
constexpr int kHelpUsageWidth = 26;

auto Text(std::string text) -> CommandResult {
  return CommandResult{std::move(text), std::nullopt};
}

auto Ok() -> CommandResult {
  return Text("ok");
}

auto Load(Database& db, const std::string& path) -> CommandResult {
  if (path.empty()) {
    return Text("error: usage: \\load <file>");
  }
  const LoadStats stats = db.Load(path);
  std::ostringstream out;
  out << "loaded " << stats.triples << (stats.triples == 1 ? " triple" : " triples") << " in "
      << stats.milliseconds << " ms";
  return Text(out.str());
}

auto Explain(Database& db, const std::string& arg) -> CommandResult {
  const std::string analyze = "analyze";
  if (StartsWith(Lower(arg), analyze)) {
    return Text(db.ExplainAnalyze(Trim(std::string_view(arg).substr(analyze.size()))));
  }
  return Text(db.Explain(arg));
}

auto Timing(Database& db, const std::string& arg) -> CommandResult {
  const std::string value = Lower(arg);
  Config config = db.GetConfig();
  if (value == "on") {
    config.timing = true;
  } else if (value == "off") {
    config.timing = false;
  } else {
    return Text("error: usage: \\timing on|off");
  }
  db.UpdateConfig(config);
  return Text(config.timing ? "timing on" : "timing off");
}

auto JoinName(JoinMethod join) -> const char* {
  switch (join) {
    case JoinMethod::kAuto:
      return "auto";
    case JoinMethod::kNestedLoop:
      return "nlj";
    case JoinMethod::kIndexNestedLoop:
      return "inlj";
    case JoinMethod::kHash:
      return "hash";
  }
  return "?";
}

auto IsolationName(IsolationLevel level) -> const char* {
  switch (level) {
    case IsolationLevel::kReadUncommitted:
      return "ru";
    case IsolationLevel::kReadCommitted:
      return "rc";
    case IsolationLevel::kRepeatableRead:
      return "rr";
    case IsolationLevel::kSerializable:
      return "ser";
  }
  return "?";
}

auto ShowSettings(const Config& config) -> CommandResult {
  std::ostringstream out;
  out << "backend " << (config.backend == BackendKind::kMem ? "mem" : "indexed") << '\n'
      << "pool_size " << config.pool_size << '\n'
      << "lru_k " << config.lru_k << '\n'
      << "join " << JoinName(config.join) << '\n'
      << "isolation " << IsolationName(config.isolation);
  return Text(out.str());
}

// Applies one knob to `config`. Returns an error message, or empty on success.
auto SetKnob(Config& config, const std::string& key, const std::string& value) -> std::string {
  if (key == "backend") {
    if (value == "indexed") {
      throw NotImplementedException("PLAN 3.4: IndexedStore");
    }
    if (value != "mem") {
      return "error: backend is mem or indexed";
    }
    config.backend = BackendKind::kMem;
  } else if (key == "pool_size") {
    config.pool_size = static_cast<size_t>(std::stoul(value));
  } else if (key == "lru_k") {
    config.lru_k = static_cast<size_t>(std::stoul(value));
    if (config.lru_k == 0) {
      return "error: lru_k must be positive";
    }
  } else if (key == "join") {
    for (const auto join : {JoinMethod::kAuto, JoinMethod::kNestedLoop,
                            JoinMethod::kIndexNestedLoop, JoinMethod::kHash}) {
      if (value == JoinName(join)) {
        config.join = join;
        return "";
      }
    }
    return "error: join is auto, nlj, inlj, or hash";
  } else if (key == "isolation") {
    for (const auto level : {IsolationLevel::kReadUncommitted, IsolationLevel::kReadCommitted,
                             IsolationLevel::kRepeatableRead, IsolationLevel::kSerializable}) {
      if (value == IsolationName(level)) {
        config.isolation = level;
        return "";
      }
    }
    return "error: isolation is ru, rc, rr, or ser";
  } else {
    return "error: unknown knob " + key;
  }
  return "";
}

auto Set(Database& db, const std::string& arg) -> CommandResult {
  if (arg.empty()) {
    return ShowSettings(db.GetConfig());
  }
  const auto space = arg.find(' ');
  if (space == std::string::npos) {
    return Text("error: usage: \\set <knob> <value>");
  }
  Config config = db.GetConfig();
  const std::string error =
      SetKnob(config, Lower(arg.substr(0, space)), Lower(Trim(arg.substr(space + 1))));
  if (!error.empty()) {
    return Text(error);
  }
  db.UpdateConfig(config);
  return Ok();
}

auto Tree(Database& db, const std::string& arg) -> CommandResult {
  const std::string lower = Lower(arg);
  std::string index = arg;
  bool dot = false;
  for (const char* name : {"spo", "pos", "osp"}) {
    if (StartsWith(lower, name)) {
      index = arg.substr(0, 3);
      dot = lower.find("dot") != std::string::npos;
    }
  }
  return Text(db.TreeDebug(index, dot));
}

auto Page(Database& db, const std::string& arg) -> CommandResult {
  if (arg.empty()) {
    return Text("error: usage: \\page <id>");
  }
  return Text(db.PageDebug(static_cast<page_id_t>(std::stoi(arg))));
}

auto Trace(Database& db, const std::string& arg) -> CommandResult {
  const std::string lower = Lower(arg);
  if (lower != "on" && lower != "off") {
    return Text("error: usage: \\trace on|off");
  }
  db.SetTracing(lower == "on");
  return Text("trace " + lower);
}

auto Log(Database& db, const std::string& arg) -> CommandResult {
  const size_t n = arg.empty() ? kDefaultLogTail : static_cast<size_t>(std::stoul(arg));
  return Text(db.LogDebug(n));
}

auto BuildCommands() -> std::vector<ShellCommand> {
  using Run = std::function<CommandResult(Database&, const std::string&)>;
  auto no_arg = [](auto fn) -> Run {
    return [fn](Database& db, const std::string&) { return fn(db); };
  };
  return {
      {"\\load", "\\load <file>", "load Turtle, N-Triples, or RDF/XML", {}, Load},
      {"\\explain",
       "\\explain <q>",
       "print the plan",
       {"\\explain analyze <q>      estimated vs actual rows (PLAN 5.5)"},
       Explain},
      {"\\stats",
       "\\stats",
       "page reads/writes, buffer hits/misses, log flushes",
       {},
       no_arg([](Database& db) { return Text(db.GetStats().Format()); })},
      {"\\timing", "\\timing on|off", "print statement time", {}, Timing},
      {"\\set",
       "\\set",
       "show settings",
       {"\\set backend mem|indexed", "\\set pool_size N", "\\set lru_k K",
        "\\set join auto|nlj|inlj|hash", "\\set isolation ru|rc|rr|ser"},
       Set},
      {"\\bpm", "\\bpm", "buffer pool frames", {}, no_arg([](Database& db) {
         return Text(db.BufferPoolDebug());
       })},
      {"\\tree", "\\tree spo|pos|osp [dot]", "index shape (indexed backend)", {}, Tree},
      {"\\page", "\\page <id>", "one page: pageLSN and the first payload bytes", {}, Page},
      {"\\trace", "\\trace on|off", "print component events to stderr", {}, Trace},
      {"\\begin", "\\begin", "start a transaction (PLAN 7.2)", {}, no_arg([](Database& db) {
         db.Begin();
         return Ok();
       })},
      {"\\commit", "\\commit", "commit the transaction (PLAN 7.2)", {}, no_arg([](Database& db) {
         db.Commit();
         return Ok();
       })},
      {"\\abort", "\\abort", "abort the transaction (PLAN 7.2)", {}, no_arg([](Database& db) {
         db.Abort();
         return Ok();
       })},
      {"\\txns", "\\txns", "running transactions (PLAN 7.2)", {}, no_arg([](Database& db) {
         return Text(db.TransactionsDebug());
       })},
      {"\\locks", "\\locks", "lock table (PLAN 7.3)", {}, no_arg([](Database& db) {
         return Text(db.LocksDebug());
       })},
      {"\\log", "\\log [n]", "decoded WAL tail", {}, Log},
      {"\\checkpoint",
       "\\checkpoint",
       "snapshot and reset the log (PLAN 9.4)",
       {},
       no_arg([](Database& db) { return Text(db.Checkpoint()); })},
      {"\\verify",
       "\\verify",
       "check snapshot, log, and state (PLAN 9.6)",
       {},
       no_arg([](Database& db) { return Text(db.Verify()); })},
      {"\\crash", "\\crash", "abrupt exit without flushing", {}, no_arg([](Database& db) {
         db.Crash();
         return Ok();
       })},
  };
}

}  // namespace

auto Commands() -> const std::vector<ShellCommand>& {
  static const std::vector<ShellCommand> commands = BuildCommands();
  return commands;
}

auto FindCommand(std::string_view word) -> const ShellCommand* {
  for (const auto& command : Commands()) {
    if (command.word == word) {
      return &command;
    }
  }
  return nullptr;
}

auto SplitCommand(const std::string& line) -> std::pair<std::string, std::string> {
  const std::string trimmed = Trim(line);
  const auto space = trimmed.find_first_of(" \t");
  if (space == std::string::npos) {
    return {Lower(trimmed), ""};
  }
  return {Lower(trimmed.substr(0, space)), Trim(std::string_view(trimmed).substr(space))};
}

auto HelpText() -> std::string {
  std::ostringstream out;
  auto line = [&](const std::string& usage, const std::string& summary) {
    out << std::left << std::setw(kHelpUsageWidth) << usage << summary << '\n';
  };
  line("SELECT / INSERT / DELETE", "SPARQL, run when the { } group closes");
  for (const auto& command : Commands()) {
    line(command.usage, command.summary);
    for (const auto& detail : command.details) {
      out << detail << '\n';
    }
  }
  line("\\session <n>", "switch session");
  line("\\help", "this list");
  line("\\quit", "leave");
  return out.str();
}

}  // namespace ontodb
