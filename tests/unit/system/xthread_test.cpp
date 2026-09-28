/**
 * @file        tests/unit/system/xthread_test.cpp
 * @brief       Unit tests for host-backed guest thread lifecycle behavior
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <atomic>
#include <filesystem>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include <rex/platform.h>
#include <rex/runtime.h>
#include <rex/system/xthread.h>

#if REX_ARCH_AMD64
#include <xmmintrin.h>
#endif

namespace {

using rex::X_STATUS;

#if REX_ARCH_AMD64

constexpr uint32_t kAllMxcsrExceptionMasks = _MM_MASK_INVALID | _MM_MASK_DENORM |
                                             _MM_MASK_DIV_ZERO | _MM_MASK_OVERFLOW |
                                             _MM_MASK_UNDERFLOW | _MM_MASK_INEXACT;

class ScopedMxcsr {
 public:
  explicit ScopedMxcsr(uint32_t value) : original_(_mm_getcsr()) { _mm_setcsr(value); }
  ~ScopedMxcsr() { _mm_setcsr(original_); }

 private:
  uint32_t original_;
};

class HostileMxcsrHostThread final : public rex::system::XHostThread {
 public:
  using XHostThread::XHostThread;

  void Execute() override {
    // Establish the regression-triggering state on the worker itself. This
    // avoids depending on platform-specific FP-state inheritance behavior.
    _mm_setcsr(_mm_getcsr() & ~_MM_MASK_INEXACT);
    XHostThread::Execute();
  }
};

#endif

}  // namespace

TEST_CASE("XHostThread initializes host floating-point exception masks",
          "[runtime][thread][fpscr]") {
#if REX_ARCH_AMD64
  const uint32_t caller_mxcsr = _mm_getcsr();
  const uint32_t hostile_mxcsr = caller_mxcsr & ~_MM_MASK_INEXACT;
  REQUIRE((hostile_mxcsr & _MM_MASK_INEXACT) == 0);

  rex::Runtime runtime(std::filesystem::current_path());
  rex::RuntimeConfig config{};
  config.tool_mode = true;
  REQUIRE(runtime.Setup(std::move(config)) == X_STATUS_SUCCESS);

  std::atomic<uint32_t> callback_mxcsr{0};
  std::atomic<bool> callback_called{false};
  auto thread = rex::system::object_ref<HostileMxcsrHostThread>(new HostileMxcsrHostThread(
      runtime.kernel_state(), 128 * 1024, rex::system::X_CREATE_SUSPENDED, [&]() {
        callback_mxcsr.store(_mm_getcsr(), std::memory_order_relaxed);
        callback_called.store(true, std::memory_order_release);
        return 0;
      }));

  REQUIRE(thread->Create() == X_STATUS_SUCCESS);
  {
    // Also make the caller state hostile before starting the worker, but don't
    // rely on thread-state inheritance for the assertion.
    ScopedMxcsr hostile_caller(hostile_mxcsr);
    REQUIRE(thread->Resume() == X_STATUS_SUCCESS);
  }

  REQUIRE(thread->Wait(0, 0, 0, nullptr) == X_STATUS_SUCCESS);
  REQUIRE(callback_called.load(std::memory_order_acquire));
  CHECK((callback_mxcsr.load(std::memory_order_relaxed) & kAllMxcsrExceptionMasks) ==
        kAllMxcsrExceptionMasks);
  CHECK(_mm_getcsr() == caller_mxcsr);
#else
  SKIP("MXCSR is only available on AMD64 hosts");
#endif
}
