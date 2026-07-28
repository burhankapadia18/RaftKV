/**
 * @file thread_pool_test.cpp
 * @brief Unit tests for ThreadPool (spec R4.10).
 *
 * The pool exists so one slow client can no longer hold the whole HTTP surface.
 * The properties that matter are not "does it run a task" but the lifetime
 * ones: every queued task must run even while stopping (each owns a client fd),
 * and a task submitted after stop() must be REFUSED rather than dropped, so its
 * caller can close that fd itself.
 *
 * These run under ThreadSanitizer in CI; a missing lock shows up there rather
 * than as a flake here.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "network/thread_pool.hpp"

namespace kvdb {
namespace {

using namespace std::chrono_literals;

TEST(ThreadPoolTest, RunsEverySubmittedTask) {
  std::atomic<int> ran{0};
  {
    ThreadPool pool(4);
    for (int i = 0; i < 500; ++i) {
      EXPECT_TRUE(pool.submit([&ran] { ran.fetch_add(1); }));
    }
    // Destructor stops and joins, which is also what guarantees the count below
    // is stable by the time it is read.
  }
  EXPECT_EQ(ran.load(), 500);
}

TEST(ThreadPoolTest, ActuallyUsesMoreThanOneThread) {
  // Without this the pool could be a serial queue and every other test would
  // still pass. Each task parks until all workers have arrived, so the test can
  // only finish if they genuinely run concurrently.
  constexpr int kWorkers = 4;
  ThreadPool pool(kWorkers);

  std::mutex mu;
  std::condition_variable cv;
  int arrived = 0;

  for (int i = 0; i < kWorkers; ++i) {
    ASSERT_TRUE(pool.submit([&] {
      std::unique_lock<std::mutex> lock(mu);
      ++arrived;
      cv.notify_all();
      // Wait for everyone. A serial pool would deadlock here, so the test has a
      // bounded wait and asserts on the count instead of hanging forever.
      cv.wait_for(lock, 5s, [&] { return arrived >= kWorkers; });
    }));
  }

  std::unique_lock<std::mutex> lock(mu);
  const bool all_arrived =
      cv.wait_for(lock, 5s, [&] { return arrived >= kWorkers; });
  EXPECT_TRUE(all_arrived)
      << "only " << arrived << " of " << kWorkers
      << " workers ran concurrently — the pool is behaving serially";
  lock.unlock();
  cv.notify_all();
}

TEST(ThreadPoolTest, StopLetsAlreadyQueuedTasksFinish) {
  // THE load-bearing property. Each queued task owns a client fd in production,
  // so exiting as soon as stopping_ is set would leak one per abandoned task.
  std::atomic<int> ran{0};
  ThreadPool pool(2);

  for (int i = 0; i < 200; ++i) {
    ASSERT_TRUE(pool.submit([&ran] {
      std::this_thread::sleep_for(1ms);
      ran.fetch_add(1);
    }));
  }

  pool.stop();
  EXPECT_EQ(ran.load(), 200)
      << "stop() abandoned queued tasks; in production each of those owns a "
         "client fd";
}

TEST(ThreadPoolTest, SubmitAfterStopIsRefusedNotDropped) {
  // A refused submit is what lets HttpServer::run close the fd it still owns.
  // Silently returning true and dropping the task would leak it.
  ThreadPool pool(2);
  pool.stop();

  std::atomic<int> ran{0};
  EXPECT_FALSE(pool.submit([&ran] { ran.fetch_add(1); }));
  EXPECT_EQ(ran.load(), 0);
}

TEST(ThreadPoolTest, StopIsIdempotentAndSafeBeforeDestruction) {
  ThreadPool pool(3);
  std::atomic<int> ran{0};
  ASSERT_TRUE(pool.submit([&ran] { ran.fetch_add(1); }));

  pool.stop();
  pool.stop(); // must not join twice or throw
  EXPECT_EQ(ran.load(), 1);
  // Destructor runs stop() a third time.
}

TEST(ThreadPoolTest, ZeroWorkersStillRunsTasks) {
  // A pool with no workers would queue every connection forever, which is worse
  // than being slow, so 0 is clamped to 1 rather than honored.
  ThreadPool pool(0);
  EXPECT_GE(pool.size(), 1u);

  std::atomic<bool> ran{false};
  ASSERT_TRUE(pool.submit([&ran] { ran.store(true); }));
  pool.stop();
  EXPECT_TRUE(ran.load());
}

TEST(ThreadPoolTest, DefaultWorkerCountIsNeverZero) {
  // hardware_concurrency() may legitimately return 0.
  EXPECT_GE(ThreadPool::default_workers(), 1u);
}

TEST(ThreadPoolTest, ConcurrentSubmittersAreSafe) {
  // The accept loop is one submitter today, but the queue is shared state and a
  // missing lock here is exactly what TSan is in CI to catch.
  constexpr int kSubmitters = 8;
  constexpr int kPerSubmitter = 200;

  std::atomic<int> ran{0};
  {
    ThreadPool pool(4);
    std::vector<std::thread> submitters;
    submitters.reserve(kSubmitters);
    for (int i = 0; i < kSubmitters; ++i) {
      submitters.emplace_back([&pool, &ran] {
        for (int j = 0; j < kPerSubmitter; ++j) {
          pool.submit([&ran] { ran.fetch_add(1); });
        }
      });
    }
    for (std::thread &t : submitters) {
      t.join();
    }
  }
  EXPECT_EQ(ran.load(), kSubmitters * kPerSubmitter);
}

} // namespace
} // namespace kvdb
