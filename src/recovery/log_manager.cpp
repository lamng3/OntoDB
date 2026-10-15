#include "recovery/log_manager.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>

#include "common/exception.h"

namespace ontodb {
namespace {

// How long the flush thread sleeps when nobody asks for a flush. Buffered
// records that no commit forced reach the disk within this interval.
constexpr auto kFlushInterval = std::chrono::milliseconds(10);

void WriteAll(int fd, const char* data, size_t len) {
  while (len > 0) {
    const ssize_t n = ::write(fd, data, len);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw StorageException(std::string("log write failed: ") + std::strerror(errno));
    }
    data += n;
    len -= static_cast<size_t>(n);
  }
}

}  // namespace

auto ScanLog(const std::filesystem::path& path) -> LogScan {
  LogScan scan;
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return scan;
  }
  const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
  scan.file_bytes = bytes.size();
  size_t off = 0;
  while (off < bytes.size()) {
    try {
      auto record = LogRecord::Deserialize(bytes.data() + off, bytes.size() - off);
      off += record.Size();
      scan.records.push_back(std::move(record));
    } catch (const LogCorruptionException& ex) {
      scan.stop = std::string(ex.what()) + " at byte " + std::to_string(off);
      break;
    } catch (const StorageException& ex) {
      scan.stop = std::string(ex.what()) + " at byte " + std::to_string(off);
      scan.torn = true;
      break;
    }
  }
  scan.valid_bytes = off;
  return scan;
}

LogManager::LogManager(std::string log_path, IoStats* stats)
    : log_path_(std::move(log_path)), stats_(stats) {
  const LogScan scan = ScanLog(log_path_);
  if (!scan.records.empty()) {
    next_lsn_ = scan.records.back().GetLsn() + 1;
    persistent_lsn_ = scan.records.back().GetLsn();
    buffered_lsn_ = persistent_lsn_;
  }
  fd_ = ::open(log_path_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd_ < 0) {
    throw StorageException("cannot open log " + log_path_ + ": " + std::strerror(errno));
  }
}

LogManager::~LogManager() {
  try {
    if (running_) {
      StopFlushThread();
    } else {
      std::unique_lock<std::mutex> lock(mu_);
      WriteBuffered(lock);
    }
  } catch (const std::exception& ex) {
    // A destructor cannot throw. The records stay unflushed, as after a crash.
    failure_ = ex.what();
  }
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

void LogManager::StartFlushThread() {
  std::lock_guard<std::mutex> guard(mu_);
  if (running_) {
    return;
  }
  running_ = true;
  stop_ = false;
  thread_ = std::thread([this] { FlushThreadLoop(); });
}

void LogManager::StopFlushThread() {
  {
    std::lock_guard<std::mutex> guard(mu_);
    if (!running_) {
      return;
    }
    stop_ = true;
  }
  work_cv_.notify_all();
  thread_.join();
  std::unique_lock<std::mutex> lock(mu_);
  running_ = false;
  WriteBuffered(lock);
}

auto LogManager::Append(LogRecord* record) -> lsn_t {
  std::lock_guard<std::mutex> guard(mu_);
  const lsn_t lsn = next_lsn_++;
  record->SetLsn(lsn);
  const size_t at = buffer_.size();
  buffer_.resize(at + record->Size());
  record->SerializeTo(buffer_.data() + at);
  buffered_lsn_ = lsn;
  return lsn;
}

void LogManager::Flush(lsn_t lsn, bool force) {
  std::unique_lock<std::mutex> lock(mu_);
  if (!running_) {
    while (persistent_lsn_ < lsn && persistent_lsn_ < buffered_lsn_) {
      if (writing_) {
        durable_cv_.wait(lock);
      } else {
        WriteBuffered(lock);
      }
    }
    return;
  }
  flush_requested_ = true;
  work_cv_.notify_one();
  if (!force) {
    return;
  }
  durable_cv_.wait(lock, [&] {
    return persistent_lsn_ >= lsn || persistent_lsn_ >= buffered_lsn_ || !failure_.empty();
  });
  if (persistent_lsn_ < lsn && !failure_.empty()) {
    throw StorageException("log flush failed: " + failure_);
  }
}

void LogManager::FlushThreadLoop() {
  std::unique_lock<std::mutex> lock(mu_);
  while (true) {
    work_cv_.wait_for(lock, kFlushInterval, [&] { return flush_requested_ || stop_; });
    flush_requested_ = false;
    try {
      WriteBuffered(lock);
    } catch (const std::exception& ex) {
      failure_ = ex.what();
      durable_cv_.notify_all();
      return;
    }
    if (stop_) {
      return;
    }
  }
}

void LogManager::WriteBuffered(std::unique_lock<std::mutex>& lock) {
  if (writing_ || buffer_.empty()) {
    return;
  }
  writing_ = true;
  std::vector<char> batch;
  batch.swap(buffer_);
  const lsn_t upto = buffered_lsn_;
  lock.unlock();
  try {
    WriteAll(fd_, batch.data(), batch.size());
    if (::fsync(fd_) != 0) {
      throw StorageException(std::string("log fsync failed: ") + std::strerror(errno));
    }
  } catch (...) {
    lock.lock();
    writing_ = false;
    durable_cv_.notify_all();
    throw;
  }
  if (stats_ != nullptr) {
    stats_->LogFlush();
  }
  lock.lock();
  writing_ = false;
  persistent_lsn_ = upto;
  flush_count_++;
  durable_cv_.notify_all();
}

auto LogManager::GetPersistentLsn() const -> lsn_t {
  std::lock_guard<std::mutex> guard(mu_);
  return persistent_lsn_;
}

auto LogManager::GetFlushCount() const -> uint64_t {
  std::lock_guard<std::mutex> guard(mu_);
  return flush_count_;
}

void LogManager::AdvanceNextLsn(lsn_t lsn) {
  std::lock_guard<std::mutex> guard(mu_);
  if (lsn > next_lsn_) {
    next_lsn_ = lsn;
  }
}

auto LogManager::NextLsn() const -> lsn_t {
  std::lock_guard<std::mutex> guard(mu_);
  return next_lsn_;
}

auto LogManager::DebugTail(size_t n) const -> std::string {
  std::vector<LogRecord> records = ScanLog(log_path_).records;
  {
    std::lock_guard<std::mutex> guard(mu_);
    size_t off = 0;
    while (off < buffer_.size()) {
      auto record = LogRecord::Deserialize(buffer_.data() + off, buffer_.size() - off);
      off += record.Size();
      records.push_back(std::move(record));
    }
  }
  const size_t start = records.size() > n ? records.size() - n : 0;
  std::ostringstream out;
  for (size_t i = start; i < records.size(); i++) {
    out << records[i].ToString();
    if (i + 1 < records.size()) {
      out << '\n';
    }
  }
  if (records.empty()) {
    out << "(empty log)";
  }
  return out.str();
}

}  // namespace ontodb
