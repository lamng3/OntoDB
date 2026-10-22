#pragma once

#include <functional>
#include <string>

namespace ontodb {

// A small Turtle and N-Triples parser for builds without raptor (the
// WebAssembly build). Each statement reaches `sink` as three canonical term
// spellings (see common/term_codec.h). Throws std::runtime_error with a line
// number on bad input. RDF/XML is not supported here.
using TurtleSink = std::function<void(const std::string& s, const std::string& p, const std::string& o)>;

void ParseTurtle(const std::string& text, const TurtleSink& sink);

}  // namespace ontodb
