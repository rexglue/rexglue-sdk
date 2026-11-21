/**
 * @file        tests/unit/benchmark_pool.h
 * @brief       Threads that a benchmark starts once and releases together for each timed round
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <vector>

#include <rex/types.h>

namespace rex::test {

class Caller;

/**
 * How long a benchmark body may run on its thread. Full sampling runs every benchmark of a case
 * inside one body, which takes longer than the 20 seconds a test body gets.
 */
inline constexpr std::chrono::seconds kBenchmarkBodyTimeout{600};

/**
 * Threads started once and released together for each round, so a timed loop holds the work and
 * not the thread start. Every thread waits at a rendezvous until all of them have arrived, then
 * runs its body, so the bodies begin together. The pool records when each body began and ended and
 * reports how much of the round all of them ran at the same time.
 */
class ThreadPool {
 public:
  /** Runs once per round on each thread. A guest pool passes the Caller bound to the thread. */
  using Body = std::function<void(int index, Caller* caller)>;

  /**
   * Starts the threads, which wait for the first round.
   *
   * @param count The number of threads.
   * @param body The work of one round on one thread.
   * @param guest A caller on a guest thread to start guest threads through, null for host threads.
   */
  ThreadPool(int count, Body body, Caller* guest = nullptr);

  /** Stops and joins every thread. */
  ~ThreadPool();

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  /** Releases every thread for one round and returns at once, so the caller can act during it. */
  void Start();

  /** Waits until every thread finished the round Start released. */
  void Wait();

  /** Runs one round: Start, then Wait. */
  void Run() {
    Start();
    Wait();
  }

  /**
   * Reports how much of a round every thread ran at the same time, averaged over the rounds
   * finished by Wait.
   *
   * @return The time from the last body start to the first body end, as a fraction of the time from
   *         the first start to the last end. 0 when the bodies never ran together.
   */
  double GetOverlap() const;

 private:
  void RunThread(int index, Caller* caller);
  double MeasureOverlap() const;

  int count_;
  Body body_;
  Caller* guest_;
  std::atomic<u32> generation_{0};
  std::atomic<u32> arrived_{0};
  std::atomic<u32> finished_{0};
  std::atomic<bool> stop_{false};
  std::vector<std::chrono::steady_clock::time_point> starts_;
  std::vector<std::chrono::steady_clock::time_point> ends_;
  std::vector<std::function<void()>> joiners_;
  double overlap_sum_ = 0.0;
  u64 overlap_rounds_ = 0;
};

}  // namespace rex::test
