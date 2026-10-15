#include "buffer/lru_k_replacer.h"

#include <stdexcept>
#include <string>

namespace ontodb {

LRUKReplacer::LRUKReplacer(size_t num_frames, size_t k, TraceSink* trace)
    : k_(k), trace_(trace), frames_(num_frames) {
  if (k == 0) {
    throw std::invalid_argument("LRU-K needs k >= 1");
  }
}

// The victim has the largest backward k-distance. A frame with fewer than k
// accesses has infinite distance; among those, the earliest oldest access
// wins. Among finite ones, the largest distance is the oldest k-th most
// recent access, which is history.front() once the history holds k entries.
// Both rules compare history.front(), so one scan decides.
auto LRUKReplacer::Evict(frame_id_t* frame_id) -> bool {
  std::lock_guard<std::mutex> guard(mu_);
  frame_id_t victim = INVALID_FRAME_ID;
  bool victim_infinite = false;
  uint64_t victim_time = 0;
  for (size_t i = 0; i < frames_.size(); i++) {
    const Frame& frame = frames_[i];
    if (!frame.evictable) {
      continue;
    }
    const bool infinite = frame.history.size() < k_;
    const uint64_t time = frame.history.empty() ? 0 : frame.history.front();
    const bool better = victim == INVALID_FRAME_ID || (infinite && !victim_infinite) ||
                        (infinite == victim_infinite && time < victim_time);
    if (better) {
      victim = static_cast<frame_id_t>(i);
      victim_infinite = infinite;
      victim_time = time;
    }
  }
  if (victim == INVALID_FRAME_ID) {
    return false;
  }
  if (trace_ != nullptr) {
    trace_->Event(
        "replacer",
        "evict frame " + std::to_string(victim) + " (k-distance " +
            (victim_infinite ? std::string("inf") : std::to_string(clock_ - victim_time)) + ")");
  }
  frames_[victim] = Frame{};
  evictable_count_--;
  *frame_id = victim;
  return true;
}

void LRUKReplacer::RecordAccess(frame_id_t frame_id) {
  std::lock_guard<std::mutex> guard(mu_);
  CheckFrame(frame_id);
  auto& history = frames_[frame_id].history;
  history.push_back(++clock_);
  if (history.size() > k_) {
    history.pop_front();
  }
}

void LRUKReplacer::SetEvictable(frame_id_t frame_id, bool set_evictable) {
  std::lock_guard<std::mutex> guard(mu_);
  CheckFrame(frame_id);
  Frame& frame = frames_[frame_id];
  if (frame.evictable != set_evictable) {
    frame.evictable = set_evictable;
    if (set_evictable) {
      evictable_count_++;
    } else {
      evictable_count_--;
    }
  }
}

void LRUKReplacer::Remove(frame_id_t frame_id) {
  std::lock_guard<std::mutex> guard(mu_);
  CheckFrame(frame_id);
  if (frames_[frame_id].evictable) {
    evictable_count_--;
  }
  frames_[frame_id] = Frame{};
}

auto LRUKReplacer::Size() const -> size_t {
  std::lock_guard<std::mutex> guard(mu_);
  return evictable_count_;
}

void LRUKReplacer::CheckFrame(frame_id_t frame_id) const {
  if (frame_id < 0 || static_cast<size_t>(frame_id) >= frames_.size()) {
    throw std::invalid_argument("frame id " + std::to_string(frame_id) + " is out of range");
  }
}

}  // namespace ontodb
