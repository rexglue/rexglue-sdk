/**
 * @file        tests/unit/core/thread_redirect_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("Redirect unwinds a thread in compiled code of the recompiled code section",
          "[core][thread]") {
  CheckRedirectUnwinds(&SpinInSection);
}

#if REX_TEST_ASM_SPINS
TEST_CASE("Redirect unwinds a thread at the first instruction of a frameless leaf",
          "[core][thread]") {
  CheckRedirectUnwinds(&rex_test_spin_leaf);
}

TEST_CASE("Redirect unwinds a thread inside a prologue", "[core][thread]") {
  CheckRedirectUnwinds(&rex_test_spin_prologue);
}
#endif

TEST_CASE("Redirect leaves a thread outside the recompiled code section running",
          "[core][thread]") {
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

TEST_CASE("A thread that ended and was waited on ignores later signals", "[core][thread]") {
  rex::platform::Thread::CreationParameters params;
  params.stack_size = 64 * 1024 * 1024;
  auto thread = rex::platform::Thread::Create(params, [] {});
  REQUIRE(rex::platform::Wait(thread.get(), false, 1s) == rex::platform::WaitResult::kSuccess);
  thread->QueueUserCallback([] {});
  CHECK_FALSE(thread->Suspend());
  CHECK_FALSE(thread->Redirect(&InRecompiledCode, nullptr, &ThrowRedirected));
}
