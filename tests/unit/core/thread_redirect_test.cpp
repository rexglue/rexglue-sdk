/**
 * @file        tests/unit/core/thread_redirect_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <span>
#include <thread>

#include <rex/platform/thread.h>
#include <rex/ppc/recompiled_code.h>

REX_RECOMPILED_CODE_SECTION extern "C" void SpinInSection(const volatile rex::u32* release) {
  while (*release == 0) {}
}

#if REX_ARCH_AMD64 && REX_PLATFORM_WIN32
extern "C" void rex_test_spin_leaf(const volatile rex::u32* release);
extern "C" void rex_test_spin_prologue(const volatile rex::u32* release);
__asm__(
    ".section rexguest,\"xr\"\n"
    ".globl rex_test_spin_leaf\n"
    ".p2align 4\n"
    "rex_test_spin_leaf:\n"
    "1: cmpl $0, (%rcx)\n"
    "je 1b\n"
    "ret\n"
    ".globl rex_test_spin_prologue\n"
    ".p2align 4\n"
    "rex_test_spin_prologue:\n"
    ".seh_proc rex_test_spin_prologue\n"
    "pushq %rbx\n"
    ".seh_pushreg %rbx\n"
    "2: cmpl $0, (%rcx)\n"
    "je 2b\n"
    ".seh_endprologue\n"
    "popq %rbx\n"
    "ret\n"
    ".seh_endproc\n"
    ".text\n");
#define REX_TEST_ASM_SPINS 1
#elif REX_ARCH_AMD64 && REX_PLATFORM_LINUX
extern "C" void rex_test_spin_leaf(const volatile rex::u32* release);
extern "C" void rex_test_spin_prologue(const volatile rex::u32* release);
__asm__(
    ".pushsection rexguest,\"ax\",@progbits\n"
    ".globl rex_test_spin_leaf\n"
    ".type rex_test_spin_leaf,@function\n"
    "rex_test_spin_leaf:\n"
    ".cfi_startproc\n"
    "1: cmpl $0, (%rdi)\n"
    "je 1b\n"
    "ret\n"
    ".cfi_endproc\n"
    ".globl rex_test_spin_prologue\n"
    ".type rex_test_spin_prologue,@function\n"
    "rex_test_spin_prologue:\n"
    ".cfi_startproc\n"
    "pushq %rbx\n"
    ".cfi_adjust_cfa_offset 8\n"
    ".cfi_offset %rbx, -16\n"
    "2: cmpl $0, (%rdi)\n"
    "je 2b\n"
    "popq %rbx\n"
    ".cfi_adjust_cfa_offset -8\n"
    "ret\n"
    ".cfi_endproc\n"
    ".popsection\n");
#define REX_TEST_ASM_SPINS 1
#endif

namespace {

using namespace std::chrono_literals;
using Spin = void (*)(const volatile rex::u32*);

struct Redirected {};

struct SetOnDestroy {
  std::atomic<bool>& flag;
  ~SetOnDestroy() { flag = true; }
};

std::atomic<bool> g_spinning{false};
std::atomic<bool> g_redirected{false};
std::atomic<bool> g_unwound{false};
std::atomic<bool> g_release{false};
volatile rex::u32 g_never_released = 0;
Spin volatile g_spin = nullptr;

void SpinOutsideSection() {
  while (!g_release.load()) {}
}

bool InRecompiledCode(rex::u64 pc, void*) {
  const std::span<const rex::u8> code = REX_RECOMPILED_CODE();
  const auto begin = reinterpret_cast<rex::u64>(code.data());
  return pc >= begin && pc < begin + code.size();
}

[[noreturn]] void ThrowRedirected() {
  throw Redirected{};
}

bool RedirectWithin(rex::platform::Thread& thread, std::chrono::milliseconds budget) {
  const auto until = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < until) {
    if (thread.Redirect(&InRecompiledCode, nullptr, &ThrowRedirected)) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return false;
}

void WaitSpinning() {
  const auto until = std::chrono::steady_clock::now() + 2s;
  while (!g_spinning.load() && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(1ms);
  }
  REQUIRE(g_spinning.load());
}

void CheckRedirectUnwinds(Spin spin) {
  REQUIRE_FALSE(REX_RECOMPILED_CODE().empty());
  g_spinning = false;
  g_redirected = false;
  g_unwound = false;
  g_spin = spin;
  auto thread = rex::platform::Thread::Create({}, [] {
    try {
      SetOnDestroy guard{g_unwound};
      g_spinning = true;
      g_spin(&g_never_released);
    } catch (const Redirected&) {
      g_redirected = true;
    }
  });
  WaitSpinning();
  std::this_thread::sleep_for(10ms);
  CHECK(RedirectWithin(*thread, 1s));
  CHECK(rex::platform::Wait(thread.get(), false, 1s) == rex::platform::WaitResult::kSuccess);
  CHECK(g_redirected.load());
  CHECK(g_unwound.load());
}

}  // namespace

REX_TEST_CASE("Redirect unwinds a thread in compiled code of the recompiled code section",
              "[core][thread][forced_stop]") {
  CheckRedirectUnwinds(&SpinInSection);
}

#if REX_TEST_ASM_SPINS
REX_TEST_CASE("Redirect unwinds a thread at the first instruction of a frameless leaf",
              "[core][thread][forced_stop]") {
  CheckRedirectUnwinds(&rex_test_spin_leaf);
}

REX_TEST_CASE("Redirect unwinds a thread inside a prologue", "[core][thread][forced_stop]") {
  CheckRedirectUnwinds(&rex_test_spin_prologue);
}
#endif

REX_TEST_CASE("Redirect leaves a thread outside the recompiled code section running",
              "[core][thread][forced_stop]") {
  g_spinning = false;
  g_release = false;
  auto thread = rex::platform::Thread::Create({}, [] {
    g_spinning = true;
    SpinOutsideSection();
  });
  WaitSpinning();
  CHECK_FALSE(RedirectWithin(*thread, 100ms));
  g_release = true;
  CHECK(rex::platform::Wait(thread.get(), false, 1s) == rex::platform::WaitResult::kSuccess);
}

REX_TEST_CASE("A thread that ended and was waited on ignores later signals", "[core][thread]") {
  rex::platform::Thread::CreationParameters params;
  params.stack_size = size_t{64} * 1024 * 1024;
  auto thread = rex::platform::Thread::Create(params, [] {});
  REQUIRE(rex::platform::Wait(thread.get(), false, 1s) == rex::platform::WaitResult::kSuccess);
  thread->QueueUserCallback([] {});
  CHECK_FALSE(thread->Suspend());
  CHECK_FALSE(thread->Redirect(&InRecompiledCode, nullptr, &ThrowRedirected));
}

REX_TEST_CASE("An alertable wait with no time left still takes a signaled object",
              "[core][thread]") {
  using rex::platform::WaitResult;
  auto ready = rex::platform::Event::CreateManualResetEvent(true);
  auto other = rex::platform::Event::CreateManualResetEvent(false);
  CHECK(rex::platform::Wait(ready.get(), true, 0ms) == WaitResult::kSuccess);
  CHECK(rex::platform::SignalAndWait(other.get(), ready.get(), true, 0ms) == WaitResult::kSuccess);
  rex::platform::WaitHandle* handles[] = {other.get(), ready.get()};
  CHECK(rex::platform::WaitAny(handles, 2, true, 0ms).first == WaitResult::kSuccess);
}

REX_TEST_CASE("A thread suspended while it suspends another thread holds no lock on it",
              "[core][thread]") {
  std::atomic<bool> stop{false};
  std::atomic<rex::u32> asked{0};
  std::atomic<rex::u32> answered{0};
  std::atomic<rex::u32> ready{0};
  auto target = rex::platform::Thread::Create({}, [&stop] {
    while (!stop.load()) {
      std::this_thread::yield();
    }
  });
  auto sender = rex::platform::Thread::Create({}, [&stop, &target, &ready] {
    ready.fetch_add(1);
    while (!stop.load()) {
      target->Suspend();
      target->Resume();
    }
  });
  std::thread probe([&] {
    ready.fetch_add(1);
    while (!stop.load()) {
      if (answered.load() < asked.load()) {
        target->Suspend();
        target->Resume();
        answered.fetch_add(1);
      }
    }
  });
  while (ready.load() < 2) {
    std::this_thread::yield();
  }
  bool answered_every_time = true;
  for (rex::u32 i = 1; i <= 200 && answered_every_time; ++i) {
    sender->Suspend();
    asked.store(i);
    const auto until = std::chrono::steady_clock::now() + 1s;
    while (answered.load() < i && std::chrono::steady_clock::now() < until) {
      std::this_thread::yield();
    }
    answered_every_time = answered.load() == i;
    sender->Resume();
  }
  stop = true;
  probe.join();
  CHECK(answered_every_time);
  CHECK(rex::platform::Wait(sender.get(), false, 1s) == rex::platform::WaitResult::kSuccess);
  CHECK(rex::platform::Wait(target.get(), false, 1s) == rex::platform::WaitResult::kSuccess);
}

/** Covers a Resume that runs before the delayed suspend handler parks and one that runs after. */
REX_TEST_CASE("A thread suspended as it exits finishes once resumed", "[core][thread]") {
  using rex::platform::WaitResult;
  bool finished_every_time = true;
  rex::platform::Thread* stuck_thread = nullptr;
  constexpr rex::u32 kPairs = 2000;
  for (rex::u32 i = 0; i < 200 && finished_every_time; ++i) {
    std::atomic<bool> go{false};
    auto thread = rex::platform::Thread::Create({}, [&go] {
      while (!go.load()) {
        std::this_thread::yield();
      }
    });
    std::thread hammer([&] {
      for (rex::u32 n = 0; n < kPairs; ++n) {
        thread->Suspend();
        thread->Resume();
      }
    });
    go = true;
    for (rex::u32 n = 0; n < kPairs; ++n) {
      thread->Suspend();
      thread->Resume();
    }
    hammer.join();
    finished_every_time = rex::platform::Wait(thread.get(), false, 30s) == WaitResult::kSuccess;
    if (!finished_every_time) {
      stuck_thread = thread.release();
    }
  }
  CHECK(finished_every_time);
  CHECK(stuck_thread == nullptr);
}

REX_TEST_CASE("A thread handle destroyed right after its body ends frees nothing still in use",
              "[core][thread]") {
  const rex::u64 before = rex::platform::detail::LiveCount();
  for (int i = 0; i < 200; ++i) {
    std::atomic<bool> done{false};
    auto thread = rex::platform::Thread::Create({}, [&done] { done.store(true); });
    REQUIRE(thread);
    while (!done.load()) {
      std::this_thread::yield();
    }
    thread.reset();
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (rex::platform::detail::LiveCount() > before &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(rex::platform::detail::LiveCount() == before);
}
