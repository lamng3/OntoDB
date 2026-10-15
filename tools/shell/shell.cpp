#include "shell/shell.h"

#include <iostream>
#include <sstream>

#include "common/exception.h"
#include "shell/commands.h"
#include "shell/text.h"

namespace ontodb {
namespace {

auto Rest(std::string_view text, std::string_view prefix) -> std::string {
  return Trim(text.substr(prefix.size()));
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
  if (!line.empty() && line[0] == '\\') {
    const auto [word, arg] = SplitCommand(line);
    const ShellCommand* command = FindCommand(word);
    if (command == nullptr) {
      return "error: unknown command (try \\help)";
    }
    const CommandResult result = command->run(db_, arg);
    return result.milliseconds ? Timed(result.text, *result.milliseconds) : result.text;
  }
  const StatementResult result = db_.Execute(line, &session);
  return Timed(FormatResult(result), result.milliseconds);
}

auto Shell::Timed(const std::string& text, double milliseconds) const -> std::string {
  if (!db_.GetConfig().timing) {
    return text;
  }
  std::ostringstream out;
  out << text;
  if (!text.empty() && text.back() != '\n') {
    out << '\n';
  }
  out << "time: " << milliseconds << " ms";
  return out.str();
}

}  // namespace ontodb
