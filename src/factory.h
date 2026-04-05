#ifndef LUNA_FACTORY_H
#define LUNA_FACTORY_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class lua_State;

namespace luna {

using PromiseId = uint64_t;

class AsyncJob {
protected:
  std::optional<std::string> error_;

public:
  virtual ~AsyncJob() = default;
  virtual void Invoke(lua_State *L) = 0;
  virtual void Run() = 0;
  virtual int Finish(lua_State *L) = 0;
  std::string &&GetError() { return std::move(*error_); }
  bool Rejected() const { return error_.has_value(); }
};

using CompletedJob = std::pair<PromiseId, std::unique_ptr<AsyncJob>>;

class CompletionQueue {
public:
  void Push(CompletedJob &&c) {
    std::lock_guard<std::mutex> lock(mu_);
    q_.push_back(std::move(c));
  }

  std::vector<CompletedJob> Drain() {
    std::vector<CompletedJob> out;
    std::lock_guard<std::mutex> lock(mu_);
    out.assign(std::make_move_iterator(q_.begin()),
               std::make_move_iterator(q_.end()));
    q_.clear();
    return out;
  }

private:
  std::mutex mu_;
  std::vector<CompletedJob> q_;
};

class Factory {
public:
  ~Factory();

  void Start();
  PromiseId EnqueueJob(std::unique_ptr<AsyncJob> &&job);
  std::vector<CompletedJob> DrainCompletions();
  void Shutdown();

private:
  static constexpr int WORKER_COUNT = 2;

  std::atomic<PromiseId> next_promise_id_{1};
  CompletionQueue completion_queue_;

  std::mutex jobs_mu_;
  std::condition_variable_any jobs_cv_;
  std::deque<CompletedJob> jobs_;
  std::vector<std::jthread> workers_;
  bool shutting_down_ = false;

  void WorkerMain(std::stop_token stop_token, int i);
};

} // namespace luna

#endif
