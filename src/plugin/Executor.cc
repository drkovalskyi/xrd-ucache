#include "Executor.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace ucache {

namespace {
uint64_t nowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
} // namespace

Executor::Executor(unsigned threads) {
  threads_.reserve(threads);
  for (unsigned i = 0; i < threads; ++i)
    threads_.emplace_back([this] { loop(); });
  for (auto& t : threads_)
    t.detach(); // instance is leaked by design; see header
  timer_ = std::thread([this] { timerLoop(); });
  timer_.detach(); // one dedicated timer thread, leaked with the singleton
}

void Executor::post(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> g(mu_);
    queue_.push_back(std::move(task));
  }
  cv_.notify_one();
}

void Executor::postAfter(uint64_t delayMs, std::function<void()> task) {
  if (delayMs == 0) {
    post(std::move(task));
    return;
  }
  uint64_t deadline = nowNs() + delayMs * 1000000ull;
  {
    std::lock_guard<std::mutex> g(tmu_);
    timers_.push(Timed{deadline, std::move(task)});
  }
  tcv_.notify_one();
}

void Executor::loop() {
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [this] { return !queue_.empty(); });
      task = std::move(queue_.front());
      queue_.pop_front();
    }
    task(); // tasks must not throw (no exceptions cross the ABI)
  }
}

void Executor::timerLoop() {
  for (;;) {
    std::function<void()> ready;
    {
      std::unique_lock<std::mutex> lk(tmu_);
      if (timers_.empty())
        tcv_.wait(lk, [this] { return !timers_.empty(); });
      uint64_t now = nowNs();
      uint64_t deadline = timers_.top().deadlineNs;
      if (deadline <= now) {
        ready = std::move(const_cast<Timed&>(timers_.top()).task);
        timers_.pop();
      } else {
        // Wait until the earliest deadline, or until a nearer one is enqueued.
        tcv_.wait_for(lk, std::chrono::nanoseconds(deadline - now));
      }
    }
    if (ready)
      post(std::move(ready)); // hand off to the work queue (mu_ not held here)
  }
}

Executor& Executor::instance(unsigned threads) {
  static Executor* exec = new Executor(
      threads ? threads
              : std::min(8u, std::max(1u, std::thread::hardware_concurrency())));
  return *exec;
}

} // namespace ucache
