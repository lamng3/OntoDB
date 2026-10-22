#include "loader/rdf_loader.h"

#ifndef ONTODB_NO_RAPTOR
#include <raptor2.h>
#endif

#include <cctype>
#include <chrono>
#include <cstring>
#include <exception>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "common/term_codec.h"
#ifdef ONTODB_NO_RAPTOR
#include "loader/turtle_parser.h"
#endif

namespace ontodb {
namespace {

#ifndef ONTODB_NO_RAPTOR
struct LoadContext {
  Dictionary* dictionary{nullptr};
  raptor_parser* parser{nullptr};
  uint64_t triples{0};
  std::vector<Triple>* batch{nullptr};  // handed to the store in one InsertBatch
  std::string error;
  std::exception_ptr failure;  // thrown by the dictionary
};

auto SyntaxFor(const std::filesystem::path& path) -> const char* {
  std::string ext = path.extension().string();
  for (char& c : ext) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  if (ext == ".ttl") {
    return "turtle";
  }
  if (ext == ".nt") {
    return "ntriples";
  }
  if (ext == ".owl" || ext == ".rdf") {
    return "rdfxml";
  }
  return nullptr;
}

auto TermString(raptor_term* term) -> std::string {
  if (term == nullptr) {
    throw std::runtime_error("RDF statement is missing a term");
  }
  switch (term->type) {
    case RAPTOR_TERM_TYPE_URI: {
      const auto* text = raptor_uri_as_string(term->value.uri);
      return CanonicalIri(reinterpret_cast<const char*>(text));
    }
    case RAPTOR_TERM_TYPE_LITERAL: {
      const auto& lit = term->value.literal;
      std::string lex(reinterpret_cast<const char*>(lit.string), lit.string_len);
      std::string datatype;
      std::string lang;
      if (lit.datatype != nullptr) {
        datatype = reinterpret_cast<const char*>(raptor_uri_as_string(lit.datatype));
      } else if (lit.language != nullptr && lit.language_len > 0) {
        lang.assign(reinterpret_cast<const char*>(lit.language), lit.language_len);
      }
      return CanonicalLiteral(lex, datatype, lang);
    }
    case RAPTOR_TERM_TYPE_BLANK: {
      const auto& blank = term->value.blank;
      return CanonicalBlank(
          std::string(reinterpret_cast<const char*>(blank.string), blank.string_len));
    }
    default:
      throw std::runtime_error("unsupported RDF term");
  }
}

// Called from inside raptor's C code, so no exception may escape: unwinding
// through C frames is undefined and skips raptor's cleanup. The first failure
// is kept, parsing is aborted, and Load rethrows it after freeing raptor.
void OnStatement(void* user, raptor_statement* statement) {
  auto* ctx = static_cast<LoadContext*>(user);
  if (ctx->failure) {
    return;
  }
  try {
    Triple triple;
    triple.subject = ctx->dictionary->Insert(TermString(statement->subject));
    triple.predicate = ctx->dictionary->Insert(TermString(statement->predicate));
    triple.object = ctx->dictionary->Insert(TermString(statement->object));
    ctx->batch->push_back(triple);
    ctx->triples++;
  } catch (...) {
    ctx->failure = std::current_exception();
    raptor_parser_parse_abort(ctx->parser);
  }
}

void OnLog(void* user, raptor_log_message* message) {
  if (message->level < RAPTOR_LOG_LEVEL_ERROR) {
    return;
  }
  auto* ctx = static_cast<LoadContext*>(user);
  if (message->text != nullptr) {
    ctx->error = message->text;
  } else {
    ctx->error = "RDF parse error";
  }
}

#endif

}  // namespace

auto RdfLoader::Load(const std::filesystem::path& path, Dictionary& dictionary,
                     TripleStore& store) const -> LoadStats {
  return LoadFiles({path}, dictionary, store);
}

auto RdfLoader::LoadFiles(const std::vector<std::filesystem::path>& paths, Dictionary& dictionary,
                          TripleStore& store) const -> LoadStats {
  const auto started = std::chrono::steady_clock::now();
  std::vector<Triple> batch;
  LoadStats stats;
  for (const auto& path : paths) {
    stats.triples += Parse(path, dictionary, &batch);
  }
  store.InsertBatch(batch);
  stats.milliseconds =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  return stats;
}

#ifdef ONTODB_NO_RAPTOR
auto RdfLoader::Parse(const std::filesystem::path& path, Dictionary& dictionary,
                      std::vector<Triple>* batch) const -> uint64_t {
  std::string ext = path.extension().string();
  for (char& c : ext) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (ext == ".owl" || ext == ".rdf") {
    throw std::runtime_error("RDF/XML is not supported in this build; use Turtle (.ttl) or N-Triples (.nt)");
  }
  if (ext != ".ttl" && ext != ".nt") {
    throw std::runtime_error("unsupported RDF extension for " + path.string() +
                             " (expected .ttl or .nt)");
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("no such file: " + path.string());
  }
  std::ostringstream text;
  text << in.rdbuf();
  uint64_t triples = 0;
  ParseTurtle(text.str(), [&](const std::string& s, const std::string& p, const std::string& o) {
    Triple triple;
    triple.subject = dictionary.Insert(s);
    triple.predicate = dictionary.Insert(p);
    triple.object = dictionary.Insert(o);
    batch->push_back(triple);
    triples++;
  });
  return triples;
}

#else

auto RdfLoader::Parse(const std::filesystem::path& path, Dictionary& dictionary,
                      std::vector<Triple>* batch) const -> uint64_t {
  const char* syntax = SyntaxFor(path);
  if (syntax == nullptr) {
    throw std::runtime_error("unsupported RDF extension for " + path.string() +
                             " (expected .ttl, .nt, .owl, or .rdf)");
  }
  if (!std::filesystem::exists(path)) {
    throw std::runtime_error("no such file: " + path.string());
  }

  LoadContext ctx;
  ctx.dictionary = &dictionary;
  ctx.batch = batch;

  raptor_world* world = raptor_new_world();
  if (world == nullptr || raptor_world_open(world) != 0) {
    raptor_free_world(world);
    throw std::runtime_error("could not initialize raptor");
  }
  raptor_world_set_log_handler(world, &ctx, OnLog);

  raptor_parser* parser = raptor_new_parser(world, syntax);
  if (parser == nullptr) {
    raptor_free_world(world);
    throw std::runtime_error(std::string("raptor has no parser for ") + syntax);
  }
  ctx.parser = parser;
  raptor_parser_set_statement_handler(parser, &ctx, OnStatement);
  raptor_parser_set_option(parser, RAPTOR_OPTION_NO_NET, nullptr, 1);
  raptor_parser_set_option(parser, RAPTOR_OPTION_LOAD_EXTERNAL_ENTITIES, nullptr, 0);

  const std::string filename = path.string();
  unsigned char* uri_string = raptor_uri_filename_to_uri_string(filename.c_str());
  raptor_uri* uri = raptor_new_uri(world, uri_string);
  raptor_uri* base = raptor_uri_copy(uri);

  const int rc = raptor_parser_parse_file(parser, uri, base);

  raptor_free_uri(base);
  raptor_free_uri(uri);
  raptor_free_memory(uri_string);
  raptor_free_parser(parser);
  raptor_free_world(world);

  if (ctx.failure) {
    std::rethrow_exception(ctx.failure);
  }
  if (rc != 0 || !ctx.error.empty()) {
    throw std::runtime_error(ctx.error.empty() ? "RDF parse failed" : ctx.error);
  }
  return ctx.triples;
}

#endif

}  // namespace ontodb
