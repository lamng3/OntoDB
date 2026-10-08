#include "shell/shell.h"

#include <cctype>
#include <iostream>
#include <sstream>

#include "common/exception.h"

namespace ontodb {
namespace {

auto Trim(std::string_view text) -> std::string {
  size_t begin = 0;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
    begin++;
  }
  size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    end--;
  }
  return std::string(text.substr(begin, end - begin));
}

auto StartsWith(std::string_view text, std::string_view prefix) -> bool {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

auto Rest(std::string_view text, std::string_view prefix) -> std::string {
  return Trim(text.substr(prefix.size()));
}

auto Lower(std::string text) -> std::string {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return text;
}

void TrackLine(std::string_view line, int* depth, bool* in_string, char* quote, bool* saw_group) {
  bool in_iri = false;
  for (size_t i = 0; i < line.size(); i++) {
    const char c = line[i];
    if (*in_string) {
      if (c == '\\' && i + 1 < line.size()) {
        i++;
        continue;
      }
      if (c == *quote) {
        *in_string = false;
      }
      continue;
    }
    if (in_iri) {
      if (c == '>') {
        in_iri = false;
      }
      continue;
    }
    if (c == '#') {
      return;
    }
    if (c == '<') {
      in_iri = true;
      continue;
    }
    if (c == '"' || c == '\'') {
      *in_string = true;
      *quote = c;
      continue;
    }
    if (c == '{') {
      ++*depth;
      *saw_group = true;
    } else if (c == '}') {
      --*depth;
    }
  }
}

auto HelpText() -> const char* {
  return "\\load <file>              load Turtle, N-Triples, or RDF/XML\n"
         "SELECT / INSERT / DELETE   SPARQL, run when the { } group closes\n"
         "\\explain <q>              print the plan\n"
         "\\explain analyze <q>      estimated vs actual rows (PLAN 5.5)\n"
         "\\stats                    page reads/writes, buffer hits/misses\n"
         "\\timing on|off            print statement time\n"
         "\\set                     show knobs\n"
         "\\set backend mem|indexed\n"
         "\\set pool_size N\n"
         "\\set lru_k K\n"
         "\\set join auto|nlj|inlj|hash\n"
         "\\set isolation ru|rc|rr|ser\n"
         "\\bpm                      buffer pool frames (PLAN 1.6)\n"
         "\\tree spo|pos|osp [dot]   index shape (PLAN 2.2)\n"
         "\\page <id>                one page (PLAN 1.6)\n"
         "\\trace on|off             operator trace (PLAN 1.6)\n"
         "\\begin \\commit \\abort    transactions (PLAN 7.2)\n"
         "\\session <n>              switch session\n"
         "\\txns                     running transactions (PLAN 7.2)\n"
         "\\locks                    lock table (PLAN 7.3)\n"
         "\\log [n]                  decoded WAL tail (PLAN 8.2)\n"
         "\\checkpoint               (PLAN 8.5)\n"
         "\\crash                    abrupt exit (PLAN 8.3)\n"
         "\\help\n"
         "\\quit\n";
}

}  // namespace

Shell::Shell(Database& db, std::istream& in, std::ostream& out) : db_(db), in_(in), out_(out) {}

void Shell::Prompt() {
  if (in_.rdbuf() == std::cin.rdbuf() && sessions_.Active() == 1) {
    out_ << "OntoDB> " << std::flush;
  } else if (in_.rdbuf() == std::cin.rdbuf()) {
    out_ << "OntoDB:" << sessions_.Active() << "> " << std::flush;
  }
}

void Shell::Run() {
  std::string pending;
  int depth = 0;
  bool in_string = false;
  char quote = 0;
  bool saw_group = false;
  std::string line;
  while (true) {
    if (pending.empty()) {
      Prompt();
    }
    if (!std::getline(in_, line)) {
      break;
    }
    const std::string trimmed = Trim(line);
    if (trimmed == "\\quit" || trimmed == "\\q") {
      return;
    }
    if (pending.empty() && trimmed.empty()) {
      continue;
    }
    if (pending.empty() && !trimmed.empty() && trimmed[0] == '\\') {
      if (!Dispatch(trimmed)) {
        return;
      }
      continue;
    }
    if (!pending.empty()) {
      pending.push_back('\n');
    }
    pending += line;
    TrackLine(line, &depth, &in_string, &quote, &saw_group);
    if (depth > 0 || in_string || !saw_group) {
      continue;
    }
    if (!Dispatch(pending)) {
      return;
    }
    pending.clear();
    depth = 0;
    saw_group = false;
  }
}

auto Shell::Dispatch(const std::string& line) -> bool {
  const std::string trimmed = Trim(line);
  if (trimmed == "\\quit" || trimmed == "\\q") {
    return false;
  }
  if (trimmed == "\\help") {
    out_ << HelpText();
    return true;
  }
  if (StartsWith(trimmed, "\\session")) {
    const std::string arg = Rest(trimmed, "\\session");
    if (arg.empty()) {
      out_ << "session " << sessions_.Active() << '\n';
      return true;
    }
    try {
      const int id = std::stoi(arg);
      if (id <= 0) {
        out_ << "error: session id must be positive\n";
        return true;
      }
      sessions_.SetActive(id);
      out_ << "session " << id << '\n';
    } catch (const std::exception&) {
      out_ << "error: session id must be an integer\n";
    }
    return true;
  }

  const std::string text = sessions_.Run(
      sessions_.Active(), [&](SessionState& session) { return Evaluate(trimmed, session); }, &out_);
  if (!text.empty()) {
    out_ << text;
    if (text.back() != '\n') {
      out_ << '\n';
    }
  }
  return true;
}

auto Shell::FormatResult(const StatementResult& result) const -> std::string {
  std::ostringstream out;
  if (!result.is_query) {
    out << result.message;
    return out.str();
  }
  if (result.columns.empty()) {
    out << "(" << result.rows.size() << (result.rows.size() == 1 ? " row)" : " rows)");
    return out.str();
  }
  for (size_t i = 0; i < result.columns.size(); i++) {
    if (i != 0) {
      out << "  ";
    }
    out << "?" << result.columns[i];
  }
  out << '\n';
  for (const auto& row : result.rows) {
    for (size_t i = 0; i < row.size(); i++) {
      if (i != 0) {
        out << "  ";
      }
      out << row[i];
    }
    out << '\n';
  }
  out << "(" << result.rows.size() << (result.rows.size() == 1 ? " row)" : " rows)");
  return out.str();
}

auto Shell::Evaluate(const std::string& line, SessionState& session) -> std::string {
  const std::string lower = Lower(line);
  auto timed = [&](std::string text, double ms) {
    if (db_.GetConfig().timing) {
      std::ostringstream out;
      out << text;
      if (!text.empty() && text.back() != '\n') {
        out << '\n';
      }
      out << "time: " << ms << " ms";
      return out.str();
    }
    return text;
  };

  if (StartsWith(lower, "\\load")) {
    const std::string path = Rest(line, "\\load");
    if (path.empty()) {
      return "error: usage: \\load <file>";
    }
    const LoadStats stats = db_.Load(path);
    std::ostringstream out;
    out << "loaded " << stats.triples << (stats.triples == 1 ? " triple" : " triples") << " in "
        << stats.milliseconds << " ms";
    return out.str();
  }
  if (StartsWith(lower, "\\explain analyze")) {
    return db_.ExplainAnalyze(Rest(line, "\\explain analyze"));
  }
  if (StartsWith(lower, "\\explain")) {
    return db_.Explain(Rest(line, "\\explain"));
  }
  if (lower == "\\stats") {
    return db_.GetStats().Format();
  }
  if (StartsWith(lower, "\\timing")) {
    const std::string arg = Lower(Rest(line, "\\timing"));
    Config config = db_.GetConfig();
    if (arg == "on") {
      config.timing = true;
    } else if (arg == "off") {
      config.timing = false;
    } else {
      return "error: usage: \\timing on|off";
    }
    db_.UpdateConfig(config);
    return config.timing ? "timing on" : "timing off";
  }
  if (lower == "\\set") {
    const Config config = db_.GetConfig();
    std::ostringstream out;
    out << "backend " << (config.backend == BackendKind::kMem ? "mem" : "indexed") << '\n';
    out << "pool_size " << config.pool_size << '\n';
    out << "lru_k " << config.lru_k << '\n';
    out << "join "
        << (config.join == JoinMethod::kAuto              ? "auto"
            : config.join == JoinMethod::kNestedLoop      ? "nlj"
            : config.join == JoinMethod::kIndexNestedLoop ? "inlj"
                                                          : "hash")
        << '\n';
    const char* isolation = "rc";
    if (config.isolation == IsolationLevel::kReadUncommitted) {
      isolation = "ru";
    } else if (config.isolation == IsolationLevel::kRepeatableRead) {
      isolation = "rr";
    } else if (config.isolation == IsolationLevel::kSerializable) {
      isolation = "ser";
    }
    out << "isolation " << isolation;
    return out.str();
  }
  if (StartsWith(lower, "\\set ")) {
    std::string arg = Trim(line.substr(5));
    const auto space = arg.find(' ');
    if (space == std::string::npos) {
      return "error: usage: \\set <knob> <value>";
    }
    const std::string key = Lower(arg.substr(0, space));
    const std::string value = Lower(Trim(arg.substr(space + 1)));
    Config config = db_.GetConfig();
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
      if (value == "auto") {
        config.join = JoinMethod::kAuto;
      } else if (value == "nlj") {
        config.join = JoinMethod::kNestedLoop;
      } else if (value == "inlj") {
        config.join = JoinMethod::kIndexNestedLoop;
      } else if (value == "hash") {
        config.join = JoinMethod::kHash;
      } else {
        return "error: join is auto, nlj, inlj, or hash";
      }
    } else if (key == "isolation") {
      if (value == "ru") {
        config.isolation = IsolationLevel::kReadUncommitted;
      } else if (value == "rc") {
        config.isolation = IsolationLevel::kReadCommitted;
      } else if (value == "rr") {
        config.isolation = IsolationLevel::kRepeatableRead;
      } else if (value == "ser") {
        config.isolation = IsolationLevel::kSerializable;
      } else {
        return "error: isolation is ru, rc, rr, or ser";
      }
    } else {
      return "error: unknown knob " + key;
    }
    db_.UpdateConfig(config);
    return "ok";
  }
  if (lower == "\\bpm") {
    return db_.BufferPoolDebug();
  }
  if (StartsWith(lower, "\\tree")) {
    const std::string arg = Rest(line, "\\tree");
    bool dot = false;
    std::string index = arg;
    if (StartsWith(Lower(arg), "spo") || StartsWith(Lower(arg), "pos") ||
        StartsWith(Lower(arg), "osp")) {
      index = arg.substr(0, 3);
      dot = Lower(arg).find("dot") != std::string::npos;
    }
    return db_.TreeDebug(index, dot);
  }
  if (StartsWith(lower, "\\page")) {
    const std::string arg = Rest(line, "\\page");
    if (arg.empty()) {
      return "error: usage: \\page <id>";
    }
    return db_.PageDebug(static_cast<page_id_t>(std::stoi(arg)));
  }
  if (StartsWith(lower, "\\trace")) {
    throw NotImplementedException("PLAN 1.6: Shell::CmdTrace");
  }
  if (lower == "\\begin") {
    db_.Begin();
    return "ok";
  }
  if (lower == "\\commit") {
    db_.Commit();
    return "ok";
  }
  if (lower == "\\abort") {
    db_.Abort();
    return "ok";
  }
  if (lower == "\\txns") {
    return db_.TransactionsDebug();
  }
  if (lower == "\\locks") {
    return db_.LocksDebug();
  }
  if (StartsWith(lower, "\\log")) {
    const std::string arg = Rest(line, "\\log");
    const size_t n = arg.empty() ? 20 : static_cast<size_t>(std::stoul(arg));
    return db_.LogDebug(n);
  }
  if (lower == "\\checkpoint") {
    db_.Checkpoint();
    return "ok";
  }
  if (lower == "\\crash") {
    db_.Crash();
    return "ok";
  }
  if (!line.empty() && line[0] == '\\') {
    return "error: unknown command (try \\help)";
  }

  const StatementResult result = db_.Execute(line, &session);
  return timed(FormatResult(result), result.milliseconds);
}

}  // namespace ontodb
