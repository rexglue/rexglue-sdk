/**
 * @file        tests/unit/kernel/overlapped_test.cpp
 * @brief       Overlapped request completion
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
#include <cstddef>
#include <thread>

#include <rex/runtime.h>
#include <rex/runtime/dispatcher.h>
#include <rex/runtime/xio.h>

#include "kernel_fixture.h"

using rex::runtime::XOVERLAPPED;
using rex::test::Caller;
using rex::test::KernelFixture;

REX_TEST_CASE("Complete writes the result, the extended error and the length", "[kernel]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 overlapped = c.Alloc(sizeof(XOVERLAPPED));
    c.GetRuntime().GetDispatcher().Complete(
        overlapped, {.result = rex::X_ERROR_SUCCESS, .extended_error = 5, .length = 64});
    REQUIRE(c.Load32(overlapped + offsetof(XOVERLAPPED, result)) == rex::X_ERROR_SUCCESS);
    REQUIRE(c.Load32(overlapped + offsetof(XOVERLAPPED, length)) == 64);
    REQUIRE(c.Load32(overlapped + offsetof(XOVERLAPPED, extended_error)) == 5);
  });
}

REX_TEST_CASE("Complete derives the extended error and the length from a failure", "[kernel]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 overlapped = c.Alloc(sizeof(XOVERLAPPED));
    c.GetRuntime().GetDispatcher().Complete(overlapped, {.result = rex::X_ERROR_NO_SUCH_USER});
    REQUIRE(c.Load32(overlapped + offsetof(XOVERLAPPED, result)) == rex::X_ERROR_NO_SUCH_USER);
    REQUIRE(c.Load32(overlapped + offsetof(XOVERLAPPED, length)) == 0xFFFFFFFF);
    REQUIRE(c.Load32(overlapped + offsetof(XOVERLAPPED, extended_error)) ==
            rex::X_ERROR_NO_SUCH_USER);
  });
}

REX_TEST_CASE("CompleteDeferred writes an explicit length on a failure", "[kernel]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 overlapped = c.Alloc(sizeof(XOVERLAPPED));
    c.GetRuntime().GetDispatcher().CompleteDeferred(overlapped, [] {
      return rex::runtime::CompletionOptions{.result = rex::X_ERROR_FUNCTION_FAILED, .length = 0};
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (c.LoadAcquire32(overlapped + offsetof(XOVERLAPPED, result)) == rex::X_ERROR_IO_PENDING &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(c.LoadAcquire32(overlapped + offsetof(XOVERLAPPED, result)) ==
            rex::X_ERROR_FUNCTION_FAILED);
    REQUIRE(c.Load32(overlapped + offsetof(XOVERLAPPED, length)) == 0);
    REQUIRE(c.Load32(overlapped + offsetof(XOVERLAPPED, extended_error)) ==
            rex::X_ERROR_FUNCTION_FAILED);
  });
}

REX_TEST_CASE("XamGetOverlappedResult sees the event set once the result is complete", "[kernel]") {
  constexpr u32 kRounds = 20000;
  constexpr u32 kQuit = 0xFFFFFFFF;
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    REQUIRE(c.Kernel("NtCreateEvent", {handle_ptr, 0, 0, 0}) == 0);
    const u32 event = c.Load32(handle_ptr);
    const u32 overlapped = c.Alloc(sizeof(XOVERLAPPED));
    const u32 length = c.Alloc(4);
    const u32 poll = c.Quad(0);
    c.Store32(overlapped + offsetof(XOVERLAPPED, event), event);

    std::atomic<u32> go{0};
    std::atomic<u32> completed{0};
    const u32 completer = c.Spawn([&](Caller& s) {
      u32 seen = 0;
      for (;;) {
        u32 round = go.load(std::memory_order_acquire);
        while (round == seen) {
          round = go.load(std::memory_order_acquire);
        }
        if (round == kQuit) {
          return;
        }
        seen = round;
        s.XAM("XMsgCompleteIORequest", {overlapped, 0, 0, 8});
        completed.store(round, std::memory_order_release);
      }
    });

    u32 wrong = 0;
    u32 unset = 0;
    for (u32 round = 1; round <= kRounds; ++round) {
      c.Store32(overlapped + offsetof(XOVERLAPPED, result), rex::X_ERROR_IO_PENDING);
      c.Kernel("NtClearEvent", {event});
      go.store(round, std::memory_order_release);
      while (c.LoadAcquire32(overlapped + offsetof(XOVERLAPPED, result)) ==
             rex::X_ERROR_IO_PENDING) {}
      if (c.XAM("XamGetOverlappedResult", {overlapped, length, 0}) != 0) {
        ++wrong;
      }
      if (c.Kernel("NtWaitForSingleObjectEx", {event, 1, 0, poll}) != 0) {
        ++unset;
      }
      while (completed.load(std::memory_order_acquire) != round) {
        std::this_thread::yield();
      }
    }
    go.store(kQuit, std::memory_order_release);
    Join(c, completer);
    CHECK(wrong == 0);
    CHECK(unset == 0);
  });
}