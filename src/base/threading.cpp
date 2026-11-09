/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <rex/assert.h>
#include <rex/platform/sync.h>
#include <rex/platform/thread.h>

namespace rex::platform {

struct Fence::Impl {
  using StateT = uint_fast32_t;
  static constexpr StateT kSigMask = static_cast<StateT>(1) << (sizeof(StateT) * 8 - 1);

  std::mutex mutex;
  std::condition_variable cond;
  std::atomic<StateT> signal_state{0};
};

Fence::Fence() : impl_(std::make_unique<Impl>()) {}

Fence::~Fence() = default;

void Fence::Signal() {
  std::unique_lock<std::mutex> lock(impl_->mutex);
  impl_->signal_state.store(impl_->signal_state.load() | Impl::kSigMask, std::memory_order_release);
  impl_->cond.notify_all();
}

void Fence::Wait() {
  std::unique_lock<std::mutex> lock(impl_->mutex);
  REX_ASSERT((impl_->signal_state.load() & ~Impl::kSigMask) < (Impl::kSigMask - 1) &&
             "Too many threads?");

  auto signal_state = impl_->signal_state.fetch_add(1) + 1;
  for (; !(signal_state & Impl::kSigMask); signal_state = impl_->signal_state.load()) {
    impl_->cond.wait(lock);
  }

  REX_ASSERT((signal_state & ~Impl::kSigMask) > 0);
  if (signal_state == (1 | Impl::kSigMask)) {
    impl_->signal_state.store(0, std::memory_order_release);
  } else {
    impl_->signal_state.store(--signal_state, std::memory_order_release);
  }
}

u32 GetLogicalProcessorCount() {
  static u32 value = 0;
  if (!value) {
    value = std::thread::hardware_concurrency();
  }
  return value;
}

thread_local u32 g_current_thread_id = UINT_MAX;

u32 GetCallerThreadID() {
  return g_current_thread_id == UINT_MAX ? GetCallerSystemThreadID() : g_current_thread_id;
}

void SetCallerThreadID(u32 id) {
  g_current_thread_id = id;
}

}  // namespace rex::platform
