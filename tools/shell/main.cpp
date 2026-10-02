#include <iostream>
#include <string>

#include "common/database.h"
#include "shell/shell.h"

int main(int argc, char** argv) {
  std::string db_path;
  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    if (arg == "--db" && i + 1 < argc) {
      db_path = argv[++i];
    } else if (!arg.empty() && arg[0] != '-') {
      db_path = arg;
    } else {
      std::cerr << "usage: ontodb [--db path]\n";
      return 2;
    }
  }

  ontodb::Database db;
  if (!db_path.empty()) {
    std::cout << "recovery not built yet (PLAN 8.6); using an empty memory database\n";
  }
  ontodb::Shell shell(db, std::cin, std::cout);
  shell.Run();
  return 0;
}
