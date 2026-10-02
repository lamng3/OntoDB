#pragma once

#include <mutex>
#include <string>

namespace ontodb {

// Per shell session. LockManager::Lock sets a reason before it waits and
// clears it after the wait, so a blocked worker does not freeze the prompt.
// The text is the part after "session N waiting: ", for example
// "X lock held by txn 5".
class SessionState {
 public:
  void SetWaiting(std::string reason) {
    std::lock_guard<std::mutex> guard(mu_);
    reason_ = std::move(reason);
  }

  void ClearWaiting() {
    std::lock_guard<std::mutex> guard(mu_);
    reason_.clear();
  }

  auto WaitReason() const -> std::string {
    std::lock_guard<std::mutex> guard(mu_);
    return reason_;
  }

 private:
  mutable std::mutex mu_;
  std::string reason_;
};

}  // namespace ontodb
