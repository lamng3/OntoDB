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

 private:
  auto Dispatch(const std::string& line) -> bool;
  auto Evaluate(const std::string& line, SessionState& session) -> std::string;
  auto FormatResult(const StatementResult& result) const -> std::string;
  void Prompt();

  Database& db_;
  std::istream& in_;
  std::ostream& out_;
  SessionPool sessions_;
};

}  // namespace ontodb
