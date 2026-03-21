#include "factory.h"

#include <stdexcept>

namespace luna {

Factory::~Factory() { Shutdown(); }

void Factory::Start() {
  if (workers_.empty()) {
    shutting_down_ = false;
    for (int i = 0; i < WORKER_COUNT; ++i) {
      workers_.emplace_back(
          [this](std::stop_token stop_token) { WorkerMain(stop_token); });
    }
  }
}

PromiseId Factory::EnqueueJob(std::unique_ptr<AsyncJob> &&job) {
  const PromiseId id = next_promise_id_.fetch_add(1);

  {
    std::lock_guard<std::mutex> lock(jobs_mu_);
    if (shutting_down_) {
      throw std::runtime_error("factory is shutting down");
    }
    jobs_.push_back({id, std::move(job)});
  }
  jobs_cv_.notify_one();
  return id;
}

std::vector<CompletedJob> Factory::DrainCompletions() {
  return completion_queue_.Drain();
}

void Factory::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(jobs_mu_);
    if (workers_.empty()) {
      shutting_down_ = true;
      return;
    }
    shutting_down_ = true;
  }

  for (auto &worker : workers_) {
    worker.request_stop();
  }
  jobs_cv_.notify_all();
  workers_.clear();
}

void Factory::WorkerMain(std::stop_token stop_token) {
  while (true) {
    CompletedJob job;

    {
      std::unique_lock<std::mutex> lock(jobs_mu_);
      jobs_cv_.wait(lock, [&]() {
        return !jobs_.empty() || stop_token.stop_requested();
      });
      if (jobs_.empty()) {
        return;
      }
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    job.second->Run();

    completion_queue_.Push(std::move(job));
  }
}

} // namespace luna
