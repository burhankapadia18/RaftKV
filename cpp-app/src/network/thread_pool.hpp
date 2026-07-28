#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace kvdb {

/**
 * @brief A fixed pool of worker threads draining a task queue (R4.10).
 *
 * Exists so the HTTP accept loop can hand a connection off instead of serving
 * it inline. Before Phase 4 the server was single-threaded, which meant one
 * slow or malicious client held the node's entire HTTP surface — the negative
 * Content-Length wedge found in Phase 1 was exactly that failure mode.
 *
 * Deliberately small: an unbounded queue, no work stealing, no priorities. The
 * work items are one-connection-each and the real backpressure is the listen
 * backlog, so anything cleverer here would be speculative.
 *
 * Lifetime: stop() (or the destructor) drains nothing — it lets the queue
 * finish, then joins. Tasks already queued DO run; that matters because each
 * task owns a client fd and dropping one would leak it.
 */
class ThreadPool {
public:
  /**
   * @brief Start @p workers threads.
   *
   * @param workers Thread count. Zero is treated as 1: a pool that cannot run
   *        anything would silently queue every connection forever, which is a
   *        far worse failure than being slow.
   */
  explicit ThreadPool(size_t workers) {
    if (workers == 0) {
      workers = 1;
    }
    threads_.reserve(workers);
    for (size_t i = 0; i < workers; ++i) {
      threads_.emplace_back([this] { run_worker(); });
    }
  }

  ~ThreadPool() { stop(); }

  // Owns threads; copying or moving it would be meaningless.
  ThreadPool(const ThreadPool &) = delete;
  ThreadPool &operator=(const ThreadPool &) = delete;
  ThreadPool(ThreadPool &&) = delete;
  ThreadPool &operator=(ThreadPool &&) = delete;

  /**
   * @brief Queue a task.
   *
   * A task submitted after stop() is refused rather than silently dropped, so a
   * caller that owns a resource (a client fd) can clean it up.
   *
   * @return false if the pool is stopping and the task was not accepted.
   */
  bool submit(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) {
        return false;
      }
      tasks_.push(std::move(task));
    }
    // Notify outside the lock: waking a worker that then immediately blocks on
    // the mutex we still hold is pure context-switch churn.
    cv_.notify_one();
    return true;
  }

  /**
   * @brief Stop accepting work, let the queue finish, then join every worker.
   *
   * Idempotent, and safe to call from the destructor after an explicit call.
   */
  void stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) {
        return;
      }
      stopping_ = true;
    }
    cv_.notify_all();

    for (std::thread &thread : threads_) {
      if (thread.joinable()) {
        thread.join();
      }
    }
    threads_.clear();
  }

  /** @brief Number of worker threads. */
  [[nodiscard]] size_t size() const { return threads_.size(); }

  /**
   * @brief A sensible default worker count.
   *
   * hardware_concurrency() is allowed to return 0 when it cannot tell, so that
   * case falls back to a small fixed pool rather than to zero workers.
   */
  [[nodiscard]] static size_t default_workers() {
    const unsigned int detected = std::thread::hardware_concurrency();
    return detected == 0 ? 4u : static_cast<size_t>(detected);
  }

private:
  std::vector<std::thread> threads_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::queue<std::function<void()>> tasks_;
  bool stopping_ = false;

  /**
   * @brief Worker loop: run queued tasks until stopping AND the queue is empty.
   *
   * The "and the queue is empty" half is load-bearing. Exiting as soon as
   * stopping_ is set would abandon queued connections, each of which owns an
   * fd.
   */
  void run_worker() {
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
        if (tasks_.empty()) {
          // Only reachable when stopping_, per the predicate above.
          return;
        }
        task = std::move(tasks_.front());
        tasks_.pop();
      }
      // Run outside the lock, or the pool would be serial.
      task();
    }
  }
};

} // namespace kvdb
