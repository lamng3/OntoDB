#include "loader/rdf_loader.h"

#include <raptor2.h>

#include <chrono>
#include <cstring>
#include <stdexcept>

#include "common/term_codec.h"

namespace ontodb {
namespace {

struct LoadContext {
  Dictionary* dictionary{nullptr};
  TripleStore* store{nullptr};
  uint64_t triples{0};
  std::string error;
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

void OnStatement(void* user, raptor_statement* statement) {
  auto* ctx = static_cast<LoadContext*>(user);
  Triple triple;
  triple.subject = ctx->dictionary->Insert(TermString(statement->subject));
  triple.predicate = ctx->dictionary->Insert(TermString(statement->predicate));
  triple.object = ctx->dictionary->Insert(TermString(statement->object));
  ctx->store->Insert(triple);
  ctx->triples++;
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

}  // namespace

auto RdfLoader::Load(const std::filesystem::path& path, Dictionary& dictionary,
                     TripleStore& store) const -> LoadStats {
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
  ctx.store = &store;

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
  raptor_parser_set_statement_handler(parser, &ctx, OnStatement);
  raptor_parser_set_option(parser, RAPTOR_OPTION_NO_NET, nullptr, 1);
  raptor_parser_set_option(parser, RAPTOR_OPTION_LOAD_EXTERNAL_ENTITIES, nullptr, 0);

  const std::string filename = path.string();
  unsigned char* uri_string = raptor_uri_filename_to_uri_string(filename.c_str());
  raptor_uri* uri = raptor_new_uri(world, uri_string);
  raptor_uri* base = raptor_uri_copy(uri);

  const auto started = std::chrono::steady_clock::now();
  const int rc = raptor_parser_parse_file(parser, uri, base);
  const auto finished = std::chrono::steady_clock::now();

  raptor_free_uri(base);
  raptor_free_uri(uri);
  raptor_free_memory(uri_string);
  raptor_free_parser(parser);
  raptor_free_world(world);

  if (rc != 0 || !ctx.error.empty()) {
    throw std::runtime_error(ctx.error.empty() ? "RDF parse failed" : ctx.error);
  }

  LoadStats stats;
  stats.triples = ctx.triples;
  stats.milliseconds = std::chrono::duration<double, std::milli>(finished - started).count();
  return stats;
}

}  // namespace ontodb
