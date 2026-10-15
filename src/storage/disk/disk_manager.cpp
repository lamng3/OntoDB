#include "storage/disk/disk_manager.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <limits>
#include <string>

#include "common/config.h"
#include "common/exception.h"

namespace ontodb {

namespace {

auto ErrnoText(const std::string& what, const std::string& file) -> std::string {
  return what + " " + file + ": " + std::strerror(errno);
}

auto Offset(page_id_t page_id) -> off_t {
  return static_cast<off_t>(page_id) * static_cast<off_t>(PAGE_SIZE);
}

}  // namespace

DiskManager::DiskManager(std::string db_file, IoStats* stats, TraceSink* trace)
    : db_file_(std::move(db_file)), stats_(stats), trace_(trace) {
  fd_ = ::open(db_file_.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd_ < 0) {
    throw StorageException(ErrnoText("cannot open", db_file_));
  }
  struct stat info {};
  if (::fstat(fd_, &info) != 0) {
    const auto message = ErrnoText("cannot stat", db_file_);
    ::close(fd_);
    throw StorageException(message);
  }
  // A torn final page (a crash while growing the file) still counts: its id
  // was handed out, and the missing bytes read back as zeros.
  const auto pages =
      (info.st_size + static_cast<off_t>(PAGE_SIZE) - 1) / static_cast<off_t>(PAGE_SIZE);
  num_pages_ = static_cast<page_id_t>(pages);
}

DiskManager::~DiskManager() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

void DiskManager::ShutDown() {
  if (fd_ < 0) {
    return;
  }
  // A failed fsync is reported, never retried: the kernel may already have
  // dropped the dirty pages, so a retry that succeeds proves nothing.
  const bool synced = ::fsync(fd_) == 0;
  const auto message = synced ? std::string() : ErrnoText("fsync failed on", db_file_);
  ::close(fd_);
  fd_ = -1;
  if (!synced) {
    throw StorageException(message);
  }
}

void DiskManager::WritePage(page_id_t page_id, const char* page_data) {
  CheckAllocated(page_id);
  size_t done = 0;
  while (done < PAGE_SIZE) {
    const auto n = ::pwrite(fd_, page_data + done, PAGE_SIZE - done,
                            Offset(page_id) + static_cast<off_t>(done));
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      throw StorageException(
          ErrnoText("cannot write page " + std::to_string(page_id) + " of", db_file_));
    }
    done += static_cast<size_t>(n);
  }
  num_writes_.fetch_add(1, std::memory_order_relaxed);
  if (stats_ != nullptr) {
    stats_->PageWrite();
  }
  Trace("write", page_id);
}

void DiskManager::ReadPage(page_id_t page_id, char* page_data) {
  CheckAllocated(page_id);
  size_t done = 0;
  while (done < PAGE_SIZE) {
    const auto n = ::pread(fd_, page_data + done, PAGE_SIZE - done,
                           Offset(page_id) + static_cast<off_t>(done));
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0) {
      throw StorageException(
          ErrnoText("cannot read page " + std::to_string(page_id) + " of", db_file_));
    }
    if (n == 0) {
      std::memset(page_data + done, 0, PAGE_SIZE - done);  // past the end: never written
      break;
    }
    done += static_cast<size_t>(n);
  }
  num_reads_.fetch_add(1, std::memory_order_relaxed);
  if (stats_ != nullptr) {
    stats_->PageRead();
  }
  Trace("read", page_id);
}

auto DiskManager::AllocatePage() -> page_id_t {
  std::lock_guard<std::mutex> guard(alloc_mu_);
  const page_id_t page_id = num_pages_.load();
  if (page_id == std::numeric_limits<page_id_t>::max()) {
    throw StorageException("database file " + db_file_ + " is full");
  }
  // Growing the file records the allocation. ftruncate extends with zeros and
  // usually writes no data blocks, so this is cheap.
  if (::ftruncate(fd_, Offset(page_id + 1)) != 0) {
    throw StorageException(ErrnoText("cannot grow", db_file_));
  }
  num_pages_.store(page_id + 1);
  Trace("allocate", page_id);
  return page_id;
}

void DiskManager::DeallocatePage(page_id_t page_id) {
  CheckAllocated(page_id);
  Trace("deallocate", page_id);
}

auto DiskManager::GetNumWrites() const -> int {
  return num_writes_.load(std::memory_order_relaxed);
}

auto DiskManager::GetNumReads() const -> int {
  return num_reads_.load(std::memory_order_relaxed);
}

void DiskManager::Trace(const char* op, page_id_t page_id) {
  if (trace_ != nullptr) {
    trace_->Event("disk", std::string(op) + " page " + std::to_string(page_id));
  }
}

void DiskManager::CheckAllocated(page_id_t page_id) const {
  if (fd_ < 0) {
    throw StorageException("database file " + db_file_ + " is shut down");
  }
  if (page_id < 0 || page_id >= num_pages_.load()) {
    throw StorageException("page " + std::to_string(page_id) + " was never allocated in " +
                           db_file_);
  }
}

}  // namespace ontodb
