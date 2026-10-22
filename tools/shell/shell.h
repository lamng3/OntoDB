#pragma once

#include <iosfwd>
#include <string>

#include "common/database.h"
#include "shell/session.h"

namespace ontodb {

class Shell {
 public:
  Shell(Database& db, std::istream& in, std::ostream& out);
  void Run();
  // Runs one command or statement on the calling thread, with no session
  // worker. The web build, which has no threads, calls this.
  auto Evaluate(const std::string& line, SessionState& session) -> std::string;

 private:
  auto Dispatch(const std::string& line) -> bool;
  auto FormatResult(const StatementResult& result) const -> std::string;
  // Appends "time: N ms" when \timing is on.
  auto Timed(const std::string& text, double milliseconds) const -> std::string;
  void Prompt();

  Database& db_;
  std::istream& in_;
  std::ostream& out_;
  SessionPool sessions_;
};

}  // namespace ontodb
