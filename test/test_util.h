#pragma once

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace ontodb::test {

inline auto SourceDir() -> std::filesystem::path {
  return ONTODB_SOURCE_DIR;
}

inline auto DataFile(const std::string& name) -> std::filesystem::path {
  return SourceDir() / "data" / name;
}

class TempDir {
 public:
  TempDir() {
    auto pattern = (std::filesystem::temp_directory_path() / "ontodb-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    char* made = mkdtemp(buffer.data());
    if (made == nullptr) {
      throw std::runtime_error("mkdtemp failed");
    }
    path_ = made;
  }

  ~TempDir() { std::filesystem::remove_all(path_); }

  TempDir(const TempDir&) = delete;
  auto operator=(const TempDir&) -> TempDir& = delete;

  auto path() const -> const std::filesystem::path& { return path_; }

 private:
  std::filesystem::path path_;
};

}  // namespace ontodb::test
