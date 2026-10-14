#include <iostream>
#include <memory>
#include <string>

#include "common/database.h"
#include "shell/shell.h"

int main(int argc, char** argv) {
  std::string db_path;
  auto mode = ontodb::RecoveryMode::kPointInTime;
  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    if (arg == "--db" && i + 1 < argc) {
      db_path = argv[++i];
    } else if (arg == "--strict") {
      mode = ontodb::RecoveryMode::kAbsoluteConsistency;
    } else if (!arg.empty() && arg[0] != '-') {
      db_path = arg;
    } else {
      std::cerr << "usage: ontodb [--strict] [--db] [dir]\n";
      return 2;
    }
  }

  std::unique_ptr<ontodb::Database> db;
  if (db_path.empty()) {
    db = std::make_unique<ontodb::Database>();
  } else {
    try {
      ontodb::OpenReport report;
      db = ontodb::Database::Open(db_path, mode, &report);
      std::cout << report.ToString() << "\n";
    } catch (const std::exception& ex) {
      std::cerr << "error: " << ex.what() << "\n";
      return 1;
    }
  }
  ontodb::Shell shell(*db, std::cin, std::cout);
  shell.Run();
  return 0;
}
