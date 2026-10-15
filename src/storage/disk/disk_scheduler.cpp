#include "storage/disk/disk_scheduler.h"

#include <exception>
#include <string>
#include <utility>

namespace ontodb {

DiskScheduler::DiskScheduler(DiskManager* disk_manager, TraceSink* trace)
    : disk_manager_(disk_manager), trace_(trace), worker_([this] { Run(); }) {}

DiskScheduler::~DiskScheduler() {
  {
    std::lock_guard<std::mutex> guard(mu_);
    queue_.emplace_back(std::nullopt);
  }
  ready_.notify_one();
  worker_.join();
}

void DiskScheduler::Schedule(DiskRequest request) {
  {
    std::lock_guard<std::mutex> guard(mu_);
    queue_.emplace_back(std::move(request));
  }
  ready_.notify_one();
}

void DiskScheduler::Run() {
  while (true) {
    std::optional<DiskRequest> request;
    {
      std::unique_lock<std::mutex> lock(mu_);
      ready_.wait(lock, [this] { return !queue_.empty(); });
      request = std::move(queue_.front());
      queue_.pop_front();
    }
    if (!request.has_value()) {
      return;  // the stop marker is behind every request scheduled before it
    }
    if (trace_ != nullptr) {
      trace_->Event("io", std::string(request->is_write ? "write" : "read") + " page " +
                              std::to_string(request->page_id));
    }
    try {
      if (request->is_write) {
        disk_manager_->WritePage(request->page_id, request->data);
      } else {
        disk_manager_->ReadPage(request->page_id, request->data);
      }
      request->callback.set_value(true);
    } catch (...) {
      request->callback.set_exception(std::current_exception());
    }
  }
}

}  // namespace ontodb
