#pragma once

#include <cstddef>

#include "common/types.h"

namespace ontodb {

// K-distance eviction over a fixed set of frames.
//
// RecordAccess appends a timestamp for that frame. A frame with fewer than k
// recorded accesses has infinite backward k-distance. Evict chooses the
// evictable frame with the largest backward k-distance, breaking ties by
// preferring the frame whose k-th most recent access is older. Evict returns
// false when every frame is non-evictable or the replacer is empty, and does
// not write `frame_id` in that case.
//
// SetEvictable(false) keeps the access history but removes the frame from the
// evictable set. Remove drops the frame and its history. Size is the number
// of evictable frames. The replacer is thread-safe. Trace component "replacer".
class LRUKReplacer {
 public:
  explicit LRUKReplacer(size_t num_frames, size_t k);

  auto Evict(frame_id_t* frame_id) -> bool;
  void RecordAccess(frame_id_t frame_id);
  void SetEvictable(frame_id_t frame_id, bool set_evictable);
  void Remove(frame_id_t frame_id);
  auto Size() const -> size_t;
};

}  // namespace ontodb
