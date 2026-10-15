#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/database.h"

namespace ontodb {

// The shell's backslash commands, as a table. A line `\word rest` runs the
// command whose word matches `\word` (case-insensitive) with `rest` as its
// argument, case kept. Adding a command is one entry in Commands(); the
// dispatcher and the help text read the same table.
//
// \quit, \help, and \session are handled by the shell itself, on the main
// thread, before a line reaches this table.
struct CommandResult {
  std::string text;
  std::optional<double> milliseconds;  // set when the command reports its own time
};

struct ShellCommand {
  std::string word;                  // "\load"
  std::string usage;                 // "\load <file>"
  std::string summary;               // one line for \help
  std::vector<std::string> details;  // extra \help lines, such as each \set knob
  std::function<CommandResult(Database&, const std::string& arg)> run;
};

auto Commands() -> const std::vector<ShellCommand>&;
auto FindCommand(std::string_view word) -> const ShellCommand*;

// Splits "\word rest" into the lowercased word and the trimmed rest.
auto SplitCommand(const std::string& line) -> std::pair<std::string, std::string>;

auto HelpText() -> std::string;

}  // namespace ontodb
