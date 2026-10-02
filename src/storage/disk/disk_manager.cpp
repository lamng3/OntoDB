#include "storage/disk/disk_manager.h"

#include "common/exception.h"

namespace ontodb {

DiskManager::DiskManager(std::string db_file, IoStats* stats, TraceSink* trace)
    : db_file_(std::move(db_file)), stats_(stats), trace_(trace) {
  throw NotImplementedException("PLAN 1.1: DiskManager::DiskManager");
}

DiskManager::~DiskManager() = default;

void DiskManager::ShutDown() {
  throw NotImplementedException("PLAN 1.1: DiskManager::ShutDown");
}

void DiskManager::WritePage(page_id_t, const char*) {
  throw NotImplementedException("PLAN 1.1: DiskManager::WritePage");
}

void DiskManager::ReadPage(page_id_t, char*) {
  throw NotImplementedException("PLAN 1.1: DiskManager::ReadPage");
}

auto DiskManager::AllocatePage() -> page_id_t {
  throw NotImplementedException("PLAN 1.1: DiskManager::AllocatePage");
}

void DiskManager::DeallocatePage(page_id_t) {
  throw NotImplementedException("PLAN 1.1: DiskManager::DeallocatePage");
}

auto DiskManager::GetNumWrites() const -> int {
  throw NotImplementedException("PLAN 1.1: DiskManager::GetNumWrites");
}

auto DiskManager::GetNumReads() const -> int {
  throw NotImplementedException("PLAN 1.1: DiskManager::GetNumReads");
}

}  // namespace ontodb
