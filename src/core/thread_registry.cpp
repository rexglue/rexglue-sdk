/**
 * @file        core/thread_registry.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "core/thread_registry.h"

#include <mutex>
#include <type_traits>
#include <unordered_set>

#include <rex/platform.h>
#include <rex/platform/thread.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace rex::platform::detail {
namespace {

std::mutex& RegistryMutex() {
  static std::mutex mutex;
  return mutex;
}

std::unordered_set<u64>& LiveThreads() {
  static std::unordered_set<u64> threads;
  return threads;
}

struct ExitGuard {
  u64 id = 0;
  ~ExitGuard() {
    if (id != 0) {
      UnregisterThread(id);
    }
  }
};

thread_local ExitGuard g_exit_guard;

}  // namespace

u64 CurrentThreadID() {
#if REX_PLATFORM_WIN32
  return ::GetCurrentThreadId();
#else
  const pthread_t self = pthread_self();
  if constexpr (std::is_pointer_v<pthread_t>) {
    return reinterpret_cast<uintptr_t>(self);
  } else {
    return static_cast<u64>(self);
  }
#endif
}

void RegisterCurrentThread() {
  if (g_exit_guard.id != 0) {
    return;
  }
  g_exit_guard.id = CurrentThreadID();
  std::lock_guard lock(RegistryMutex());
  LiveThreads().insert(g_exit_guard.id);
}

void UnregisterThread(u64 id) {
  std::lock_guard lock(RegistryMutex());
  LiveThreads().erase(id);
}

u64 LiveCount() {
  std::lock_guard lock(RegistryMutex());
  return LiveThreads().size();
}

}  // namespace rex::platform::detail
