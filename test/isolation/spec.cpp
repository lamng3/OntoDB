#include "isolation/spec.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ontodb::test {
namespace {

auto Trim(std::string text) -> std::string {
  size_t begin = 0;
  while (begin < text.size() &&
         (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r')) {
    begin++;
  }
  size_t end = text.size();
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) {
    end--;
  }
  return text.substr(begin, end - begin);
}

auto Starts(const std::string& text, const char* prefix) -> bool {
  const auto n = std::char_traits<char>::length(prefix);
  return text.size() >= n && text.compare(0, n, prefix) == 0;
}

auto IndexOf(const std::vector<std::string>& steps, const std::string& step) -> int {
  for (size_t i = 0; i < steps.size(); i++) {
    if (steps[i] == step) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void CheckOutput(const Permutation& perm, const std::string& step, const std::string& output,
                 std::string* error) {
  for (const auto& item : perm.expect) {
    if (item.step != step) {
      continue;
    }
    if (item.kind == Expectation::Kind::kAbort && output.find("abort") == std::string::npos) {
      *error = step + " expected abort, got: " + output;
    } else if (item.kind == Expectation::Kind::kContains &&
               output.find(item.argument) == std::string::npos) {
      *error = step + " missing \"" + item.argument + "\" in: " + output;
    } else if (item.kind == Expectation::Kind::kEquals && output != item.argument) {
      *error = step + " expected \"" + item.argument + "\" got: " + output;
    } else if (item.kind == Expectation::Kind::kAbsent &&
               output.find(item.argument) != std::string::npos) {
      *error = step + " unexpectedly contains \"" + item.argument + "\"";
    }
    if (!error->empty()) {
      return;
    }
  }
}

}  // namespace

auto Spec::SessionOf(const std::string& ref) -> int {
  const auto dot = ref.find('.');
  if (dot == std::string::npos) {
    throw std::runtime_error("step ref needs a session: " + ref);
  }
  return std::stoi(ref.substr(0, dot));
}

auto Spec::NameOf(const std::string& ref) -> std::string {
  const auto dot = ref.find('.');
  if (dot == std::string::npos) {
    throw std::runtime_error("step ref needs a name: " + ref);
  }
  return ref.substr(dot + 1);
}

auto Spec::StepText(const std::string& ref) const -> std::string {
  const auto session = sessions.find(SessionOf(ref));
  if (session == sessions.end()) {
    throw std::runtime_error("unknown session in " + ref);
  }
  const auto step = session->second.find(NameOf(ref));
  if (step == session->second.end()) {
    throw std::runtime_error("unknown step " + ref);
  }
  return step->second;
}

auto ParseSpec(const std::string& text) -> Spec {
  Spec spec;
  enum class Mode { kNone, kSetup, kSession, kPerm, kExpect };
  Mode mode = Mode::kNone;
  int session = 0;
  Permutation* perm = nullptr;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    line = Trim(line);
    if (line.empty() || line[0] == '#') {
      continue;
    }
    if (Starts(line, "name ")) {
      spec.name = Trim(line.substr(5));
      mode = Mode::kNone;
    } else if (Starts(line, "isolation ")) {
      spec.isolation = Trim(line.substr(10));
      mode = Mode::kNone;
    } else if (line == "setup") {
      mode = Mode::kSetup;
    } else if (Starts(line, "session ")) {
      session = std::stoi(Trim(line.substr(8)));
      spec.sessions[session];
      mode = Mode::kSession;
    } else if (Starts(line, "permutation ")) {
      spec.permutations.push_back(Permutation{});
      perm = &spec.permutations.back();
      perm->name = Trim(line.substr(12));
      mode = Mode::kPerm;
    } else if (Starts(line, "expect ")) {
      const auto name = Trim(line.substr(7));
      perm = nullptr;
      for (auto& item : spec.permutations) {
        if (item.name == name) {
          perm = &item;
        }
      }
      if (perm == nullptr) {
        throw std::runtime_error("expect without permutation " + name);
      }
      mode = Mode::kExpect;
    } else if (mode == Mode::kSetup) {
      if (!spec.setup.empty()) {
        spec.setup.push_back('\n');
      }
      spec.setup += line;
    } else if (mode == Mode::kSession) {
      const auto colon = line.find(':');
      if (colon == std::string::npos) {
        throw std::runtime_error("step needs a colon: " + line);
      }
      spec.sessions[session][Trim(line.substr(0, colon))] = Trim(line.substr(colon + 1));
    } else if (mode == Mode::kPerm) {
      perm->steps.push_back(line);
    } else if (mode == Mode::kExpect) {
      const auto space = line.find(' ');
      if (space == std::string::npos) {
        throw std::runtime_error("expect needs a verb: " + line);
      }
      Expectation item;
      item.step = line.substr(0, space);
      const auto rest = Trim(line.substr(space + 1));
      const auto verb_end = rest.find(' ');
      const auto verb = verb_end == std::string::npos ? rest : rest.substr(0, verb_end);
      const auto arg =
          verb_end == std::string::npos ? std::string() : Trim(rest.substr(verb_end + 1));
      if (verb == "blocked") {
        item.kind = Expectation::Kind::kBlocked;
      } else if (verb == "unblocks") {
        item.kind = Expectation::Kind::kUnblocks;
        item.argument = arg;
      } else if (verb == "abort") {
        item.kind = Expectation::Kind::kAbort;
      } else if (verb == "contains") {
        item.kind = Expectation::Kind::kContains;
        item.argument = arg;
      } else if (verb == "equals") {
        item.kind = Expectation::Kind::kEquals;
        item.argument = arg;
      } else if (verb == "absent") {
        item.kind = Expectation::Kind::kAbsent;
        item.argument = arg;
      } else {
        throw std::runtime_error("unknown expect verb " + verb);
      }
      perm->expect.push_back(std::move(item));
    } else {
      throw std::runtime_error("unexpected line: " + line);
    }
  }
  return spec;
}

auto LoadSpec(const std::filesystem::path& path) -> Spec {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("cannot read " + path.string());
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return ParseSpec(buffer.str());
}

auto Validate(const Spec& spec) -> std::string {
  if (spec.name.empty()) {
    return "missing name";
  }
  if (spec.isolation != "ru" && spec.isolation != "rc" && spec.isolation != "rr" &&
      spec.isolation != "ser") {
    return "isolation must be ru, rc, rr, or ser";
  }
  if (spec.sessions.empty() || spec.permutations.empty()) {
    return "spec needs a session and a permutation";
  }
  for (const auto& perm : spec.permutations) {
    if (perm.steps.empty()) {
      return "empty permutation " + perm.name;
    }
    for (const auto& step : perm.steps) {
      try {
        (void)spec.StepText(step);
      } catch (const std::exception& ex) {
        return ex.what();
      }
    }
    for (const auto& item : perm.expect) {
      if (IndexOf(perm.steps, item.step) < 0) {
        return "expect mentions " + item.step + " outside the permutation";
      }
      if (item.kind == Expectation::Kind::kUnblocks) {
        const int blocked_at = IndexOf(perm.steps, item.argument);
        const int at = IndexOf(perm.steps, item.step);
        if (blocked_at < 0) {
          return "unblocks unknown step " + item.argument;
        }
        if (blocked_at > at) {
          return item.step + " unblocks " + item.argument + " before that step runs";
        }
        bool marked = false;
        for (const auto& other : perm.expect) {
          if (other.step == item.argument && other.kind == Expectation::Kind::kBlocked) {
            marked = true;
          }
        }
        if (!marked) {
          return item.argument + " is unblocked but not marked blocked";
        }
      }
    }
    for (const auto& item : perm.expect) {
      if (item.kind != Expectation::Kind::kBlocked) {
        continue;
      }
      bool released = false;
      for (const auto& other : perm.expect) {
        if (other.kind == Expectation::Kind::kUnblocks && other.argument == item.step) {
          released = true;
        }
      }
      if (!released) {
        return item.step + " blocks with no unblocks";
      }
    }
  }
  return {};
}

auto RunPermutation(const Spec& spec, const Permutation& perm,
                    IsolationDriver* driver) -> RunReport {
  for (const auto& step : perm.steps) {
    const int session = Spec::SessionOf(step);
    driver->Submit(session, step, spec.StepText(step));
    bool blocked = false;
    for (const auto& item : perm.expect) {
      if (item.step == step && item.kind == Expectation::Kind::kBlocked) {
        blocked = true;
      }
    }
    if (blocked) {
      if (driver->Poll(session, std::chrono::milliseconds(50)).has_value()) {
        return {false, step + " was expected to block"};
      }
      continue;
    }
    const auto output = driver->Poll(session, std::chrono::milliseconds(1000));
    if (!output.has_value()) {
      return {false, step + " timed out"};
    }
    std::string error;
    CheckOutput(perm, step, *output, &error);
    if (!error.empty()) {
      return {false, error};
    }
    for (const auto& item : perm.expect) {
      if (item.step != step || item.kind != Expectation::Kind::kUnblocks) {
        continue;
      }
      const auto released =
          driver->Poll(Spec::SessionOf(item.argument), std::chrono::milliseconds(1000));
      if (!released.has_value()) {
        return {false, step + " did not unblock " + item.argument};
      }
      CheckOutput(perm, item.argument, *released, &error);
      if (!error.empty()) {
        return {false, error};
      }
    }
  }
  return {true, {}};
}

void ScriptedDriver::BlockUntil(const std::string& step, const std::string& until) {
  block_until_[step] = until;
}

void ScriptedDriver::SetOutput(const std::string& step, const std::string& output) {
  outputs_[step] = output;
}

void ScriptedDriver::Submit(int session, const std::string& step, const std::string&) {
  Item item;
  item.step = step;
  const auto output = outputs_.find(step);
  item.output = output == outputs_.end() ? "ok" : output->second;
  const auto until = block_until_.find(step);
  item.waiting = until != block_until_.end() && !released_[until->second];
  inbox_[session] = item;
}

auto ScriptedDriver::Poll(int session, std::chrono::milliseconds) -> std::optional<std::string> {
  auto& item = inbox_.at(session);
  if (item.waiting) {
    const auto until = block_until_.at(item.step);
    if (!released_[until]) {
      return std::nullopt;
    }
    item.waiting = false;
  }
  released_[item.step] = true;
  return item.output;
}

}  // namespace ontodb::test
