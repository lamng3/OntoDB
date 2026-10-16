#include "concurrency/lock_manager.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <set>
#include <sstream>

#include "common/exception.h"

namespace ontodb {

namespace {

constexpr auto kDetectInterval = std::chrono::milliseconds(20);

// Rows: held. Columns: requested. Order: IS, IX, S, X, SIX (the enum's order).
constexpr bool kCompatible[5][5] = {
    // IS     IX     S      X      SIX
    {true, true, true, false, true},      // IS
    {true, true, false, false, false},    // IX
    {true, false, true, false, false},    // S
    {false, false, false, false, false},  // X
    {true, false, false, false, false},   // SIX
};

auto Compatible(LockMode held, LockMode requested) -> bool {
  return kCompatible[static_cast<int>(held)][static_cast<int>(requested)];
}

// Whether holding `held` already gives everything `wanted` would.
auto Covers(LockMode held, LockMode wanted) -> bool {
  if (held == wanted || held == LockMode::kX) {
    return true;
  }
  switch (held) {
    case LockMode::kSix:
      return wanted == LockMode::kIs || wanted == LockMode::kIx || wanted == LockMode::kS;
    case LockMode::kS:
    case LockMode::kIx:
      return wanted == LockMode::kIs;
    default:
      return false;
  }
}

// The weakest mode that gives both: S with IX is SIX, anything with X is X.
auto Combine(LockMode a, LockMode b) -> LockMode {
  if (Covers(a, b)) {
    return a;
  }
  if (Covers(b, a)) {
    return b;
  }
  const bool has_s =
      a == LockMode::kS || b == LockMode::kS || a == LockMode::kSix || b == LockMode::kSix;
  const bool has_ix =
      a == LockMode::kIx || b == LockMode::kIx || a == LockMode::kSix || b == LockMode::kSix;
  if (has_s && has_ix) {
    return LockMode::kSix;
  }
  return LockMode::kX;
}

auto ModeName(LockMode mode) -> const char* {
  switch (mode) {
    case LockMode::kIs:
      return "IS";
    case LockMode::kIx:
      return "IX";
    case LockMode::kS:
      return "S";
    case LockMode::kX:
      return "X";
    case LockMode::kSix:
      return "SIX";
  }
  return "?";
}

auto ResourceText(const LockResource& resource) -> std::string {
  switch (resource.type) {
    case ResourceType::kDatabase:
      return "database";
    case ResourceType::kIndex:
      return "index " + resource.name;
    case ResourceType::kKey: {
      term_id_t s = 0;
      term_id_t p = 0;
      term_id_t o = 0;
      resource.key.Decode(&s, &p, &o);
      return resource.name + " key (" + std::to_string(s) + "," + std::to_string(p) + "," +
             std::to_string(o) + ")";
    }
  }
  return "?";
}

}  // namespace

LockManager::~LockManager() {
  StopDeadlockDetection();
}

auto LockManager::KeyOf(const LockResource& resource) -> std::string {
  std::string key(1, static_cast<char>(resource.type));
  key += resource.name;
  key.push_back('\0');
  if (resource.type == ResourceType::kKey) {
    key.append(reinterpret_cast<const char*>(resource.key.Data()), TripleKey::SIZE);
  }
  return key;
}

auto LockManager::Grantable(const Queue& queue,
                            std::list<Request>::const_iterator request) const -> bool {
  for (auto it = queue.requests.begin(); it != queue.requests.end(); ++it) {
    if (it == request) {
      continue;
    }
    if (it->id == request->id) {
      continue;  // its own lock (the one an upgrade replaces)
    }
    if (it->granted && !Compatible(it->mode, request->mode)) {
      return false;
    }
    if (!it->granted && std::distance(queue.requests.begin(), it) <
                            std::distance(queue.requests.begin(), request)) {
      return false;  // FIFO: an earlier request is still waiting
    }
  }
  return true;
}

auto LockManager::Lock(Transaction* txn, const LockResource& resource, LockMode mode,
                       SessionState* session, bool* waited) -> bool {
  if (waited != nullptr) {
    *waited = false;
  }
  if (txn->GetState() == TransactionState::kAborted) {
    throw TransactionAbortException("txn " + std::to_string(txn->GetId()) + " is aborted");
  }
  if (txn->GetState() == TransactionState::kCommitted) {
    throw std::logic_error("txn " + std::to_string(txn->GetId()) + " has committed");
  }
  std::unique_lock<std::mutex> lock(mu_);
  Queue& queue = table_[KeyOf(resource)];
  queue.resource = resource;
  auto held = std::find_if(queue.requests.begin(), queue.requests.end(),
                           [&](const Request& r) { return r.id == txn->GetId() && r.granted; });
  LockMode wanted = mode;
  if (held != queue.requests.end()) {
    if (Covers(held->mode, mode)) {
      return true;
    }
    wanted = Combine(held->mode, mode);  // an upgrade, queued behind any waiter
  }
  auto request =
      queue.requests.insert(queue.requests.end(), Request{txn->GetId(), txn, wanted, false});
  if (!Grantable(queue, request)) {
    if (waited != nullptr) {
      *waited = true;
    }
    if (session != nullptr) {
      for (const auto& other : queue.requests) {
        if (other.granted && other.id != txn->GetId() && !Compatible(other.mode, wanted)) {
          session->SetWaiting(std::string(ModeName(other.mode)) + " lock held by txn " +
                              std::to_string(other.id));
          break;
        }
      }
    }
    waiting_[txn->GetId()] = &queue;
    queue.cv.wait(lock, [&] {
      return txn->GetState() == TransactionState::kAborted || Grantable(queue, request);
    });
    waiting_.erase(txn->GetId());
    if (session != nullptr) {
      session->ClearWaiting();
    }
    if (txn->GetState() == TransactionState::kAborted) {
      queue.requests.erase(request);
      queue.cv.notify_all();
      throw TransactionAbortException("txn " + std::to_string(txn->GetId()) +
                                      " was chosen to break a deadlock");
    }
  }
  request->granted = true;
  if (held != queue.requests.end()) {
    queue.requests.erase(held);
  }
  return true;
}

auto LockManager::Unlock(Transaction* txn, const LockResource& resource) -> bool {
  std::lock_guard<std::mutex> lock(mu_);
  const auto found = table_.find(KeyOf(resource));
  if (found == table_.end()) {
    return false;
  }
  Queue& queue = found->second;
  const auto before = queue.requests.size();
  queue.requests.remove_if([&](const Request& r) { return r.id == txn->GetId() && r.granted; });
  const bool removed = queue.requests.size() != before;
  queue.cv.notify_all();
  return removed;
}

void LockManager::UnlockAll(Transaction* txn) {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto& [key, queue] : table_) {
    const auto before = queue.requests.size();
    queue.requests.remove_if([&](const Request& r) { return r.id == txn->GetId() && r.granted; });
    if (queue.requests.size() != before) {
      queue.cv.notify_all();
    }
  }
}

auto LockManager::GetLocks(txn_id_t id) const -> std::vector<std::pair<LockResource, LockMode>> {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<std::pair<LockResource, LockMode>> held;
  for (const auto& [key, queue] : table_) {
    for (const auto& request : queue.requests) {
      if (request.granted && request.id == id) {
        held.emplace_back(queue.resource, request.mode);
      }
    }
  }
  return held;
}

auto LockManager::DebugString() const -> std::string {
  std::lock_guard<std::mutex> lock(mu_);
  std::ostringstream out;
  for (const auto& [key, queue] : table_) {
    if (queue.requests.empty()) {
      continue;
    }
    out << ResourceText(queue.resource) << ":";
    for (const auto& request : queue.requests) {
      out << " txn " << request.id << ' ' << ModeName(request.mode)
          << (request.granted ? "" : " (waiting)");
    }
    out << '\n';
  }
  return out.str();
}

void LockManager::StartDeadlockDetection() {
  if (detecting_.exchange(true)) {
    return;
  }
  detector_ = std::thread([this] { DetectLoop(); });
}

void LockManager::StopDeadlockDetection() {
  if (!detecting_.exchange(false)) {
    return;
  }
  if (detector_.joinable()) {
    detector_.join();
  }
}

void LockManager::DetectLoop() {
  while (detecting_.load()) {
    std::this_thread::sleep_for(kDetectInterval);
    std::lock_guard<std::mutex> lock(mu_);
    while (Transaction* victim = FindVictim()) {
      victim->SetState(TransactionState::kAborted);
      if (const auto it = waiting_.find(victim->GetId()); it != waiting_.end()) {
        it->second->cv.notify_all();
      }
      waiting_.erase(victim->GetId());  // its edges leave the graph now
    }
  }
}

// Builds the waits-for graph and returns the youngest (largest id) member of
// the first cycle found, searching from the smallest id so results repeat.
auto LockManager::FindVictim() -> Transaction* {
  std::map<txn_id_t, std::set<txn_id_t>> edges;
  std::map<txn_id_t, Transaction*> txns;
  for (const auto& [id, queue] : waiting_) {
    for (auto w = queue->requests.begin(); w != queue->requests.end(); ++w) {
      if (w->granted || w->id != id || w->txn->GetState() == TransactionState::kAborted) {
        continue;
      }
      txns[id] = w->txn;
      for (auto other = queue->requests.begin(); other != queue->requests.end(); ++other) {
        if (other->id == w->id) {
          continue;
        }
        const bool ahead = std::distance(queue->requests.begin(), other) <
                           std::distance(queue->requests.begin(), w);
        if ((other->granted && !Compatible(other->mode, w->mode)) || (!other->granted && ahead)) {
          edges[id].insert(other->id);
        }
      }
    }
  }
  std::map<txn_id_t, int> color;  // 0 unseen, 1 on the stack, 2 done
  std::vector<txn_id_t> stack;
  std::function<std::vector<txn_id_t>(txn_id_t)> visit =
      [&](txn_id_t node) -> std::vector<txn_id_t> {
    color[node] = 1;
    stack.push_back(node);
    for (const txn_id_t next : edges[node]) {
      if (color[next] == 1) {
        return {std::find(stack.begin(), stack.end(), next), stack.end()};
      }
      if (color[next] == 0) {
        if (auto cycle = visit(next); !cycle.empty()) {
          return cycle;
        }
      }
    }
    stack.pop_back();
    color[node] = 2;
    return {};
  };
  for (const auto& [node, unused] : edges) {
    if (color[node] == 0) {
      stack.clear();
      if (const auto cycle = visit(node); !cycle.empty()) {
        // Every member of a cycle is waiting, so each has a live Transaction.
        return txns[*std::max_element(cycle.begin(), cycle.end())];
      }
    }
  }
  return nullptr;
}

}  // namespace ontodb
