// TSan-facing concurrency test for Executor: the new
// postAfter timer thread + min-deadline queue must hand every task to the pool
// exactly once under concurrent scheduling, with no races on its two locks.
// Executor is XrdCl-free, so this runs in the core unit-test binary and is
// exercised automatically by the TSan gate.
//
// Completion is signalled through a heap Latch captured BY VALUE (shared_ptr):
// its mutex gives a real happens-before edge from each task back to the waiter,
// and the heap lifetime means a task on the leaked process-wide Executor never
// touches a freed/reused stack slot. (A bare relaxed atomic on a stack local
// false-races under TSan across tests — a test artifact, since production tasks
// capture st_, a shared_ptr, never stack locals.)
#include "Executor.h"

#include <chrono>
#include <condition_variable>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace ucache;

namespace {
struct Latch {
  std::mutex m;
  std::condition_variable cv;
  int count = 0;
  void bump() {
    {
      std::lock_guard<std::mutex> g(m);
      ++count;
    }
    cv.notify_all();
  }
  bool wait(int target, int maxMs = 10000) {
    std::unique_lock<std::mutex> lk(m);
    return cv.wait_for(lk, std::chrono::milliseconds(maxMs), [&] { return count >= target; });
  }
  int get() {
    std::lock_guard<std::mutex> g(m);
    return count;
  }
};
} // namespace

TEST(Executor, PostAfterRunsEveryTaskOnce) {
  auto& ex = Executor::instance();
  auto latch = std::make_shared<Latch>();
  const int N = 500;
  for (int i = 0; i < N; ++i)
    ex.postAfter(i % 10, [latch] { latch->bump(); });
  EXPECT_TRUE(latch->wait(N));
  EXPECT_EQ(latch->get(), N);
}

TEST(Executor, PostAfterConcurrentSchedulersNoRace) {
  auto& ex = Executor::instance();
  auto latch = std::make_shared<Latch>();
  const int T = 8, PER = 200;
  std::vector<std::thread> ts;
  for (int t = 0; t < T; ++t)
    ts.emplace_back([&ex, latch] {
      for (int i = 0; i < PER; ++i)
        ex.postAfter(i % 5, [latch] { latch->bump(); });
    });
  for (auto& th : ts)
    th.join();
  EXPECT_TRUE(latch->wait(T * PER));
  EXPECT_EQ(latch->get(), T * PER);
}

TEST(Executor, PostAfterReleasesByDeadlineNotSubmissionOrder) {
  auto& ex = Executor::instance();
  struct Rec {
    std::mutex m;
    std::condition_variable cv;
    int order = 0, first = 0, second = 0;
  };
  auto rec = std::make_shared<Rec>();
  ex.postAfter(250, [rec] { // submitted first, longer delay
    std::lock_guard<std::mutex> g(rec->m);
    rec->first = ++rec->order;
    rec->cv.notify_all();
  });
  ex.postAfter(20, [rec] { // submitted second, shorter delay
    std::lock_guard<std::mutex> g(rec->m);
    rec->second = ++rec->order;
    rec->cv.notify_all();
  });
  std::unique_lock<std::mutex> lk(rec->m);
  ASSERT_TRUE(
      rec->cv.wait_for(lk, std::chrono::seconds(5), [&] { return rec->first && rec->second; }));
  EXPECT_EQ(rec->second, 1); // shorter deadline released first
  EXPECT_EQ(rec->first, 2);
}

TEST(Executor, PostAfterZeroDelayPostsImmediately) {
  auto& ex = Executor::instance();
  auto latch = std::make_shared<Latch>();
  for (int i = 0; i < 100; ++i)
    ex.postAfter(0, [latch] { latch->bump(); });
  EXPECT_TRUE(latch->wait(100));
  EXPECT_EQ(latch->get(), 100);
}
