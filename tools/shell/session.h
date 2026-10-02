#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <iosfwd>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "common/session_state.h"

namespace ontodb {

// One worker thread per session. Run submits a job and waits. While the job
// is blocked it prints "session N waiting: ..." from SessionState, then keeps
// the prompt free to talk to other sessions.
class SessionPool {
 public:
  SessionPool();
  ~SessionPool();

  void SetActive(int id);
  auto Active() const -> int { return active_; }

  using Job = std::function<std::string(SessionState&)>;

  auto Run(int session, const Job& job, std::ostream* wait_log) -> std::string;

  // Start returns once the worker has the job. WaitFor returns the result, or
  // nullopt if the job is still running after `timeout`.
  void Start(int session, const Job& job);
  auto WaitFor(int session, std::chrono::milliseconds timeout) -> std::optional<std::string>;
  auto WaitReason(int session) -> std::string;

 private:
  struct Worker {
    std::thread thread;
    std::mutex mu;
    std::condition_variable cv;
    std::function<std::string(SessionState&)> job;
    bool has_job{false};
    bool stop{false};
    bool done{false};
    std::string result;
    SessionState state;
  };

  void Ensure(int id);
  void Loop(Worker* worker);

  int active_{1};
  std::mutex mu_;
  std::map<int, std::unique_ptr<Worker>> workers_;
};

}  // namespace ontodb
