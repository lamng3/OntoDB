#pragma once

#include <stdexcept>
#include <string>

namespace ontodb {

// Thrown by every stub. `what()` is "PLAN x.y: Class::Method".
// PlanItem() is the "PLAN x.y" prefix the shell prints.
class NotImplementedException : public std::logic_error {
 public:
  explicit NotImplementedException(const std::string& what)
      : std::logic_error(what), plan_item_(CutPlan(what)) {}

  auto PlanItem() const -> const std::string& { return plan_item_; }

 private:
  static auto CutPlan(const std::string& what) -> std::string {
    const auto pos = what.find(':');
    if (pos == std::string::npos) {
      return what;
    }
    return what.substr(0, pos);
  }

  std::string plan_item_;
};

// Parse and bind errors. The shell prints line and column; never a crash.
class LocatedException : public std::runtime_error {
 public:
  LocatedException(int line, int column, const std::string& message)
      : std::runtime_error(message), line_(line), column_(column) {}

  auto line() const -> int { return line_; }
  auto column() const -> int { return column_; }

 private:
  int line_;
  int column_;
};

class ParseException : public LocatedException {
 public:
  using LocatedException::LocatedException;
};

class BindException : public LocatedException {
 public:
  using LocatedException::LocatedException;
};

class StorageException : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Deadlock detection aborts the younger transaction by throwing this from the
// lock wait. The shell and the isolation harness treat it as an abort.
class TransactionAbortException : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Armed crash points throw this. It is not a storage failure.
class CrashInjected : public std::runtime_error {
 public:
  explicit CrashInjected(const std::string& point) : std::runtime_error(point) {}
};

}  // namespace ontodb
