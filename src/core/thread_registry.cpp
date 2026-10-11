/**
 * @file        core/thread_registry.cpp
 * @brief       Count of live host threads that registered with the platform layer
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <atomic>

#include <rex/platform/thread.h>

namespace rex::platform::detail {
namespace {

constinit std::atomic<u64> g_live_count{0};

struct ExitGuard {
  bool registered = false;
  ~ExitGuard() {
    if (registered) {
      g_live_count.fetch_sub(1, std::memory_order_acq_rel);
    }
  }
};

thread_local ExitGuard g_exit_guard;

}  // namespace

void RegisterCurrentThread() {
  if (g_exit_guard.registered) {
    return;
  }
  g_exit_guard.registered = true;
  g_live_count.fetch_add(1, std::memory_order_acq_rel);
}

u64 ThreadCount() {
  return g_live_count.load(std::memory_order_acquire);
}

}  // namespace rex::platform::detail
