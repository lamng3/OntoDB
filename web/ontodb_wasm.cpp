// The web build's entry point: one Database, one Shell, one session, and a
// single exported function that runs a shell line or SPARQL statement and
// returns the text the shell would print. No threads, no stdin.
#include <emscripten/emscripten.h>

#include <memory>
#include <sstream>
#include <string>

#include "common/database.h"
#include "common/exception.h"
#include "shell/shell.h"

namespace {

std::istringstream g_in;
std::ostringstream g_out;
std::unique_ptr<ontodb::Database> g_db;
std::unique_ptr<ontodb::Shell> g_shell;
std::unique_ptr<ontodb::SessionState> g_session;
std::string g_result;

void EnsureOpen() {
  if (g_db == nullptr) {
    g_db = std::make_unique<ontodb::Database>();
    g_shell = std::make_unique<ontodb::Shell>(*g_db, g_in, g_out);
    g_session = std::make_unique<ontodb::SessionState>();
  }
}

}  // namespace

extern "C" {

// Runs one line (or a whole multi-line statement). Errors come back as text,
// worded as the native shell words them. The pointer is valid until the next call.
EMSCRIPTEN_KEEPALIVE auto ontodb_exec(const char* text) -> const char* {
  try {
    EnsureOpen();
    g_result = g_shell->Evaluate(text, *g_session);
  } catch (const ontodb::NotImplementedException& ex) {
    g_result = "not built yet (" + ex.PlanItem() + ")";
  } catch (const ontodb::LocatedException& ex) {
    g_result = "error: " + std::string(ex.what()) + " at line " + std::to_string(ex.line()) +
               ", column " + std::to_string(ex.column());
  } catch (const ontodb::TransactionAbortException& ex) {
    g_result = std::string("abort: ") + ex.what();
  } catch (const std::exception& ex) {
    g_result = std::string("error: ") + ex.what();
  }
  return g_result.c_str();
}

// A fresh, empty database.
EMSCRIPTEN_KEEPALIVE void ontodb_reset() {
  g_shell.reset();
  g_db.reset();
  g_session.reset();
  EnsureOpen();
}

}
