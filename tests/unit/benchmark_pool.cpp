/**
 * @file        tests/unit/benchmark_pool.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "benchmark_pool.h"

#include <algorithm>
#include <memory>
#include <thread>
#include <utility>

#include "kernel/kernel_fixture.h"

namespace rex::test {

ThreadPool::ThreadPool(int count, Body body, Caller* guest)
    : count_(count),
      body_(std::move(body)),
      guest_(guest),
      starts_(static_cast<size_t>(count)),
      ends_(static_cast<size_t>(count)) {
  joiners_.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    if (guest_) {
      const u32 handle = guest_->Spawn([this, i](Caller& caller) { RunThread(i, &caller); });
      joiners_.emplace_back([caller = guest_, handle] {
        caller->Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, caller->AllocQuad(-100000000)});
        caller->Kernel("NtClose", {handle});
      });
    } else {
      auto thread = std::make_shared<std::thread>([this, i] { RunThread(i, nullptr); });
      joiners_.emplace_back([thread] { thread->join(); });
    }
  }
}

ThreadPool::~ThreadPool() {
  stop_.store(true, std::memory_order_release);
  generation_.fetch_add(1, std::memory_order_release);
  generation_.notify_all();
  for (const auto& join : joiners_) {
    join();
  }
}

void ThreadPool::Start() {
  arrived_.store(0, std::memory_order_relaxed);
  finished_.store(0, std::memory_order_relaxed);
  generation_.fetch_add(1, std::memory_order_release);
  generation_.notify_all();
}

void ThreadPool::Wait() {
  const u32 total = static_cast<u32>(count_);
  for (u32 done = finished_.load(std::memory_order_acquire); done < total;
       done = finished_.load(std::memory_order_acquire)) {
    finished_.wait(done, std::memory_order_acquire);
  }
  overlap_sum_ += MeasureOverlap();
  ++overlap_rounds_;
}

void ThreadPool::RunThread(int index, Caller* caller) {
  const u32 total = static_cast<u32>(count_);
  u32 seen = 0;
  for (;;) {
    u32 generation = generation_.load(std::memory_order_acquire);
    while (generation == seen) {
      generation_.wait(seen, std::memory_order_acquire);
      generation = generation_.load(std::memory_order_acquire);
    }
    seen = generation;
    if (stop_.load(std::memory_order_acquire)) {
      return;
    }
    arrived_.fetch_add(1, std::memory_order_acq_rel);
    // Spin so the threads leave the rendezvous within nanoseconds of each other, and yield only
    // after a long wait, when more threads than cpus have to share one.
    for (u32 spins = 0; arrived_.load(std::memory_order_acquire) < total; ++spins) {
      if (spins > 50000) {
        std::this_thread::yield();
      }
    }
    const auto slot = static_cast<size_t>(index);
    starts_[slot] = std::chrono::steady_clock::now();
    body_(index, caller);
    ends_[slot] = std::chrono::steady_clock::now();
    if (finished_.fetch_add(1, std::memory_order_acq_rel) + 1 == total) {
      finished_.notify_all();
    }
  }
}

double ThreadPool::GetOverlap() const {
  return overlap_rounds_ ? overlap_sum_ / static_cast<double>(overlap_rounds_) : 0.0;
}

double ThreadPool::MeasureOverlap() const {
  const auto first_start = *std::ranges::min_element(starts_);
  const auto last_start = *std::ranges::max_element(starts_);
  const auto first_end = *std::ranges::min_element(ends_);
  const auto last_end = *std::ranges::max_element(ends_);
  const double span = std::chrono::duration<double>(last_end - first_start).count();
  const double together = std::chrono::duration<double>(first_end - last_start).count();
  return span > 0.0 && together > 0.0 ? together / span : 0.0;
}

}  // namespace rex::test
