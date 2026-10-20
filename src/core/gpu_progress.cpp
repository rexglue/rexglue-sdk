/**
 * @file        core/gpu_progress.cpp
 * @brief       See rex/graphics/gpu_progress.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/graphics/gpu_progress.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace rex::graphics::progress {

namespace {
std::atomic<uint64_t> g_count{0};
std::atomic<uint32_t> g_waiters{0};
std::mutex g_mutex;
std::condition_variable g_wake;
}  // namespace

uint64_t Count() {
  return g_count.load(std::memory_order_acquire);
}

void Signal() {
  // Sequentially consistent on both sides (here and in Wait): either the
  // waiter sees the new count before it sleeps, or this sees the waiter and
  // wakes it. Taking the mutex makes sure a waiter between its check and its
  // sleep is asleep before the notification.
  g_count.fetch_add(1, std::memory_order_seq_cst);
  if (g_waiters.load(std::memory_order_seq_cst) == 0) return;
  { std::lock_guard<std::mutex> lock(g_mutex); }
  g_wake.notify_all();
}

uint64_t Wait(uint64_t seen, uint32_t timeout_us) {
  g_waiters.fetch_add(1, std::memory_order_seq_cst);
  {
    std::unique_lock<std::mutex> lock(g_mutex);
    g_wake.wait_for(lock, std::chrono::microseconds(timeout_us),
                    [seen] { return g_count.load(std::memory_order_seq_cst) != seen; });
  }
  g_waiters.fetch_sub(1, std::memory_order_relaxed);
  return g_count.load(std::memory_order_acquire);
}

}  // namespace rex::graphics::progress
