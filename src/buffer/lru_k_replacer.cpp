#include "buffer/lru_k_replacer.h"

#include "common/exception.h"

namespace ontodb {

LRUKReplacer::LRUKReplacer(size_t, size_t) {
  throw NotImplementedException("PLAN 1.3: LRUKReplacer::LRUKReplacer");
}

auto LRUKReplacer::Evict(frame_id_t*) -> bool {
  throw NotImplementedException("PLAN 1.3: LRUKReplacer::Evict");
}

void LRUKReplacer::RecordAccess(frame_id_t) {
  throw NotImplementedException("PLAN 1.3: LRUKReplacer::RecordAccess");
}

void LRUKReplacer::SetEvictable(frame_id_t, bool) {
  throw NotImplementedException("PLAN 1.3: LRUKReplacer::SetEvictable");
}

void LRUKReplacer::Remove(frame_id_t) {
  throw NotImplementedException("PLAN 1.3: LRUKReplacer::Remove");
}

auto LRUKReplacer::Size() const -> size_t {
  throw NotImplementedException("PLAN 1.3: LRUKReplacer::Size");
}

}  // namespace ontodb
