#include "shell/session.h"

#include <chrono>
#include <iostream>
#include <utility>

#include "common/exception.h"

namespace ontodb {

SessionPool::SessionPool() {
  Ensure(1);
}

SessionPool::~SessionPool() {
  std::map<int, std::unique_ptr<Worker>> workers;
  {
    std::lock_guard<std::mutex> guard(mu_);
    workers.swap(workers_);
  }
  for (auto& entry : workers) {
    Worker* worker = entry.second.get();
    {
      std::lock_guard<std::mutex> guard(worker->mu);
      worker->stop = true;
    }
    worker->cv.notify_all();
    if (worker->thread.joinable()) {
      worker->thread.join();
    }
  }
}

void SessionPool::SetActive(int id) {
  Ensure(id);
  active_ = id;
}

void SessionPool::Ensure(int id) {
  std::lock_guard<std::mutex> guard(mu_);
  if (workers_.contains(id)) {
    return;
  }
  auto worker = std::make_unique<Worker>();
  Worker* raw = worker.get();
  raw->thread = std::thread([this, raw] { Loop(raw); });
  workers_.emplace(id, std::move(worker));
}

void SessionPool::Loop(Worker* worker) {
  while (true) {
    std::function<std::string(SessionState&)> job;
    {
      std::unique_lock<std::mutex> lock(worker->mu);
      worker->cv.wait(lock, [&] { return worker->has_job || worker->stop; });
      if (worker->stop && !worker->has_job) {
        return;
      }
      job = std::move(worker->job);
      worker->has_job = false;
    }
    std::string result;
    try {
      result = job(worker->state);
    } catch (const NotImplementedException& ex) {
      result = "not built yet (" + ex.PlanItem() + ")";
    } catch (const LocatedException& ex) {
      result = "error: " + std::string(ex.what()) + " at line " + std::to_string(ex.line()) +
               ", column " + std::to_string(ex.column());
    } catch (const TransactionAbortException& ex) {
      result = std::string("abort: ") + ex.what();
    } catch (const std::exception& ex) {
      result = std::string("error: ") + ex.what();
    }
    {
      std::lock_guard<std::mutex> lock(worker->mu);
      worker->result = std::move(result);
      worker->done = true;
    }
    worker->cv.notify_all();
  }
}

void SessionPool::Start(int session, const Job& job) {
  Ensure(session);
  Worker* worker = nullptr;
  {
    std::lock_guard<std::mutex> guard(mu_);
    worker = workers_.at(session).get();
  }
  {
    std::lock_guard<std::mutex> lock(worker->mu);
    worker->done = false;
    worker->result.clear();
    worker->job = job;
    worker->has_job = true;
  }
  worker->state.ClearWaiting();
  worker->cv.notify_all();
}

auto SessionPool::WaitFor(int session, std::chrono::milliseconds timeout)
    -> std::optional<std::string> {
  Worker* worker = nullptr;
  {
    std::lock_guard<std::mutex> guard(mu_);
    worker = workers_.at(session).get();
  }
  std::unique_lock<std::mutex> lock(worker->mu);
  if (!worker->cv.wait_for(lock, timeout, [&] { return worker->done; })) {
    return std::nullopt;
  }
  return worker->result;
}

auto SessionPool::WaitReason(int session) -> std::string {
  Worker* worker = nullptr;
  {
    std::lock_guard<std::mutex> guard(mu_);
    worker = workers_.at(session).get();
  }
  return worker->state.WaitReason();
}

auto SessionPool::Run(int session, const Job& job, std::ostream* wait_log) -> std::string {
  Ensure(session);
  Worker* worker = nullptr;
  {
    std::lock_guard<std::mutex> guard(mu_);
    worker = workers_.at(session).get();
  }
  {
    std::lock_guard<std::mutex> lock(worker->mu);
    worker->done = false;
    worker->result.clear();
    worker->job = job;
    worker->has_job = true;
  }
  worker->state.ClearWaiting();
  worker->cv.notify_all();

  std::string announced;
  while (true) {
    std::unique_lock<std::mutex> lock(worker->mu);
    worker->cv.wait_for(lock, std::chrono::milliseconds(40), [&] { return worker->done; });
    if (worker->done) {
      return worker->result;
    }
    lock.unlock();
    const std::string reason = worker->state.WaitReason();
    if (wait_log != nullptr && !reason.empty() && reason != announced) {
      *wait_log << "session " << session << " waiting: " << reason << '\n';
      announced = reason;
    }
  }
}

}  // namespace ontodb
