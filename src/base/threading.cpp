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

  std::mutex mutex_;
  std::condition_variable cond_;
  std::atomic<StateT> signal_state_{0};
};

Fence::Fence() : impl_(std::make_unique<Impl>()) {}

Fence::~Fence() = default;

void Fence::Signal() {
  std::unique_lock<std::mutex> lock(impl_->mutex_);
  impl_->signal_state_.store(impl_->signal_state_.load() | Impl::kSigMask,
                             std::memory_order_release);
  impl_->cond_.notify_all();
}

void Fence::Wait() {
  std::unique_lock<std::mutex> lock(impl_->mutex_);
  REX_ASSERT((impl_->signal_state_.load() & ~Impl::kSigMask) < (Impl::kSigMask - 1) &&
             "Too many threads?");

  auto signal_state = impl_->signal_state_.fetch_add(1) + 1;
  for (; !(signal_state & Impl::kSigMask); signal_state = impl_->signal_state_.load()) {
    impl_->cond_.wait(lock);
  }

  REX_ASSERT((signal_state & ~Impl::kSigMask) > 0);
  if (signal_state == (1 | Impl::kSigMask)) {
    impl_->signal_state_.store(0, std::memory_order_release);
  } else {
    impl_->signal_state_.store(--signal_state, std::memory_order_release);
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
