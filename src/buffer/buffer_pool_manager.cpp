#include "buffer/buffer_pool_manager.h"

#include "common/exception.h"

namespace ontodb {

BufferPoolManager::BufferPoolManager(size_t pool_size, DiskManager* disk_manager, size_t replacer_k,
                                     LogManager* log_manager, IoStats* stats, TraceSink* trace)
    : pool_size_(pool_size)
    , disk_manager_(disk_manager)
    , replacer_k_(replacer_k)
    , log_manager_(log_manager)
    , stats_(stats)
    , trace_(trace) {
  throw NotImplementedException("PLAN 1.4: BufferPoolManager::BufferPoolManager");
}

BufferPoolManager::~BufferPoolManager() = default;

auto BufferPoolManager::NewPage(page_id_t*) -> Page* {
  throw NotImplementedException("PLAN 1.4: BufferPoolManager::NewPage");
}

auto BufferPoolManager::FetchPage(page_id_t) -> Page* {
  throw NotImplementedException("PLAN 1.4: BufferPoolManager::FetchPage");
}

auto BufferPoolManager::UnpinPage(page_id_t, bool) -> bool {
  throw NotImplementedException("PLAN 1.4: BufferPoolManager::UnpinPage");
}

auto BufferPoolManager::FlushPage(page_id_t) -> bool {
  throw NotImplementedException("PLAN 1.4: BufferPoolManager::FlushPage");
}

void BufferPoolManager::FlushAllPages() {
  throw NotImplementedException("PLAN 1.4: BufferPoolManager::FlushAllPages");
}

auto BufferPoolManager::DeletePage(page_id_t) -> bool {
  throw NotImplementedException("PLAN 1.4: BufferPoolManager::DeletePage");
}

auto BufferPoolManager::GetPinnedFrameCount() const -> size_t {
  throw NotImplementedException("PLAN 1.4: BufferPoolManager::GetPinnedFrameCount");
}

auto BufferPoolManager::DebugString() const -> std::string {
  throw NotImplementedException("PLAN 1.6: BufferPoolManager::DebugString");
}

auto BufferPoolManager::FetchPageRead(page_id_t) -> ReadPageGuard {
  throw NotImplementedException("PLAN 1.5: BufferPoolManager::FetchPageRead");
}

auto BufferPoolManager::FetchPageWrite(page_id_t) -> WritePageGuard {
  throw NotImplementedException("PLAN 1.5: BufferPoolManager::FetchPageWrite");
}

auto BufferPoolManager::NewPageGuarded(page_id_t*) -> WritePageGuard {
  throw NotImplementedException("PLAN 1.5: BufferPoolManager::NewPageGuarded");
}

}  // namespace ontodb
