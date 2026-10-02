#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ontodb::test {

struct Expectation {
  enum class Kind { kBlocked, kUnblocks, kAbort, kContains, kEquals, kAbsent };
  Kind kind{Kind::kContains};
  std::string step;
  std::string argument;
};

struct Permutation {
  std::string name;
  std::vector<std::string> steps;
  std::vector<Expectation> expect;
};

struct Spec {
  std::string name;
  std::string isolation;
  std::string setup;
  std::map<int, std::map<std::string, std::string>> sessions;
  std::vector<Permutation> permutations;

  auto StepText(const std::string& ref) const -> std::string;
  static auto SessionOf(const std::string& ref) -> int;
  static auto NameOf(const std::string& ref) -> std::string;
};

auto ParseSpec(const std::string& text) -> Spec;
auto LoadSpec(const std::filesystem::path& path) -> Spec;
auto Validate(const Spec& spec) -> std::string;

class IsolationDriver {
 public:
  virtual ~IsolationDriver() = default;
  virtual void Submit(int session, const std::string& step, const std::string& text) = 0;
  virtual auto Poll(int session,
                    std::chrono::milliseconds timeout) -> std::optional<std::string> = 0;
};

struct RunReport {
  bool ok{false};
  std::string message;
};

auto RunPermutation(const Spec& spec, const Permutation& perm,
                    IsolationDriver* driver) -> RunReport;

class ScriptedDriver : public IsolationDriver {
 public:
  void BlockUntil(const std::string& step, const std::string& until);
  void SetOutput(const std::string& step, const std::string& output);
  void Submit(int session, const std::string& step, const std::string& text) override;
  auto Poll(int session, std::chrono::milliseconds timeout) -> std::optional<std::string> override;

 private:
  struct Item {
    std::string step;
    std::string output;
    bool waiting{false};
  };
  std::map<std::string, std::string> block_until_;
  std::map<std::string, std::string> outputs_;
  std::map<int, Item> inbox_;
  std::map<std::string, bool> released_;
};

}  // namespace ontodb::test
