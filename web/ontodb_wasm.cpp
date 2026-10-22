// The web build's entry point: one Database, one Shell, one session, and a
// single exported function that runs a shell line or SPARQL statement and
// returns the text the shell would print. No threads, no stdin.
#include <emscripten/emscripten.h>

#include <cstdio>
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

void AppendJson(std::string* out, const std::string& text) {
  out->push_back('"');
  for (const char c : text) {
    switch (c) {
      case '"': *out += "\\\""; break;
      case '\\': *out += "\\\\"; break;
      case '\n': *out += "\\n"; break;
      case '\r': *out += "\\r"; break;
      case '\t': *out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          *out += buf;
        } else {
          out->push_back(c);
        }
    }
  }
  out->push_back('"');
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

// Runs one SPARQL statement and returns the result as JSON:
//   {"query":true,"columns":[...],"rows":[[...]],"ms":1.2}  for SELECT and ASK
//   {"query":false,"message":"...","ms":1.2}                for updates
// Errors are {"error":"..."}. Shell commands (a backslash first) go through
// ontodb_exec instead.
EMSCRIPTEN_KEEPALIVE auto ontodb_query(const char* text) -> const char* {
  std::string error;
  try {
    EnsureOpen();
    const ontodb::StatementResult result = g_db->Execute(text, g_session.get());
    std::string& out = g_result;
    out = result.is_query ? "{\"query\":true,\"columns\":[" : "{\"query\":false,\"message\":";
    if (result.is_query) {
      for (size_t i = 0; i < result.columns.size(); i++) {
        out += i ? "," : "";
        AppendJson(&out, result.columns[i]);
      }
      out += "],\"rows\":[";
      for (size_t r = 0; r < result.rows.size(); r++) {
        out += r ? ",[" : "[";
        for (size_t i = 0; i < result.rows[r].size(); i++) {
          out += i ? "," : "";
          AppendJson(&out, result.rows[r][i]);
        }
        out += "]";
      }
      out += "]";
    } else {
      AppendJson(&out, result.message);
    }
    out += ",\"ms\":" + std::to_string(result.milliseconds) + "}";
    return out.c_str();
  } catch (const ontodb::LocatedException& ex) {
    error = std::string(ex.what()) + " at line " + std::to_string(ex.line()) + ", column " +
            std::to_string(ex.column());
  } catch (const ontodb::NotImplementedException& ex) {
    error = "not built yet (" + ex.PlanItem() + ")";
  } catch (const std::exception& ex) {
    error = ex.what();
  }
  g_result = "{\"error\":";
  AppendJson(&g_result, error);
  g_result += "}";
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
