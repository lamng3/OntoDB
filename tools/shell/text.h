#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace ontodb {

// Small string helpers shared by the shell's reader and its commands.

inline auto Trim(std::string_view text) -> std::string {
  size_t begin = 0;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
    begin++;
  }
  size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    end--;
  }
  return std::string(text.substr(begin, end - begin));
}

inline auto StartsWith(std::string_view text, std::string_view prefix) -> bool {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

inline auto Lower(std::string text) -> std::string {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return text;
}

}  // namespace ontodb
