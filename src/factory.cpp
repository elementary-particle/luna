#include "factory.h"

namespace luna {

void Factory::Start() {
  if (workers_.empty()) {
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
    jobs_.push_back({id, std::move(job)});
  }
  jobs_cv_.notify_one();
  return id;
}

std::vector<CompletedJob> Factory::DrainCompletions() {
  return completion_queue_.Drain();
}

void Factory::WorkerMain(std::stop_token stop_token) {
  while (!stop_token.stop_requested()) {
    CompletedJob job;

    {
      std::unique_lock<std::mutex> lock(jobs_mu_);
      jobs_cv_.wait(lock, stop_token, [&]() { return !jobs_.empty(); });
      if (stop_token.stop_requested())
        return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    job.second->Run();

    completion_queue_.Push(std::move(job));
  }
}

} // namespace luna
