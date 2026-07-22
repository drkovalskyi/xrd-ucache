// Internal executor: all disk I/O and buffer assembly for the
// cache run here — never on the caller's or XrdCl's callback threads.
//
// Thread-safety: fully thread-safe. The process-wide instance is
// intentionally leaked (threads end with the process): joining at static
// destruction could deadlock against XrdCl teardown.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace ucache {

class Executor {
 public:
  explicit Executor(unsigned threads);

  void post(std::function<void()> task);

  // Run `task` on the pool after at least `delayMs`, via a single dedicated
  // timer thread draining a min-deadline queue (delayed
  // dispatch). Used for open-retry backoff so worker threads are never blocked
  // sleeping — a sleep on a worker would starve the pool under mass-concurrent
  // open failures, exactly the scenario retry targets. delayMs == 0 posts now.
  void postAfter(uint64_t delayMs, std::function<void()> task);

  // Process-wide instance, sized per UCACHE_THREADS (0 = min(8, hw)).
  static Executor& instance(unsigned threads = 0);

 private:
  void loop();
  void timerLoop();

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> queue_;
  std::vector<std::thread> threads_;

  // Delayed dispatch: one timer thread drains a min-deadline queue and forwards
  // due tasks to post(). Separate lock so scheduling never contends the work
  // queue.
  struct Timed {
    uint64_t deadlineNs;
    std::function<void()> task;
    bool operator<(const Timed& o) const { return deadlineNs > o.deadlineNs; } // min-heap
  };
  std::mutex tmu_;
  std::condition_variable tcv_;
  std::priority_queue<Timed> timers_;
  std::thread timer_;
};

} // namespace ucache
