/**
 * @file        tests/unit/kernel/char_wait_test.cpp
 * @brief       Relative and absolute wait timeouts
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include <rex/runtime/xtypes.h>

#include "kernel_fixture.h"

using rex::X_STATUS;

using rex::testing::Caller;
using rex::testing::KernelFixture;

namespace {

constexpr u32 kTimeout = rex::X_STATUS_TIMEOUT;
constexpr i64 kTicksPerMs = 10000;

u64 SystemTime(Caller& c) {
  const u32 time = c.Alloc(8);
  c.Kernel("KeQuerySystemTime", {time});
  return (static_cast<u64>(c.Load32(time)) << 32) | c.Load32(time + 4);
}

u32 Event(Caller& c, u32 initial_state) {
  const u32 handle = c.Alloc(4);
  REQUIRE(c.Kernel("NtCreateEvent", {handle, 0, 0, initial_state}) == 0);
  return c.Load32(handle);
}

i64 ElapsedMs(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                               start)
      .count();
}

}  // namespace

TEST_CASE("waits: an absolute deadline in the past times out at once", "[char][waits]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 event = Event(c, 0);
    const u32 deadline = c.Quad(static_cast<i64>(SystemTime(c)) - 1000 * kTicksPerMs);
    const auto start = std::chrono::steady_clock::now();
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {event, 1, 0, deadline}) == kTimeout);
    CHECK(ElapsedMs(start) < 500);
    c.Kernel("NtClose", {event});
  });
}

TEST_CASE("waits: an absolute deadline of now times out at once", "[char][waits]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 event = Event(c, 0);
    const u32 deadline = c.Quad(static_cast<i64>(SystemTime(c)));
    const auto start = std::chrono::steady_clock::now();
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {event, 1, 0, deadline}) == kTimeout);
    CHECK(ElapsedMs(start) < 500);
    c.Kernel("NtClose", {event});
  });
}

TEST_CASE("waits: an absolute deadline in the future waits until it", "[char][waits]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 event = Event(c, 0);
    const u32 deadline = c.Quad(static_cast<i64>(SystemTime(c)) + 100 * kTicksPerMs);
    const auto start = std::chrono::steady_clock::now();
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {event, 1, 0, deadline}) == kTimeout);
    const i64 elapsed = ElapsedMs(start);
    CHECK(elapsed >= 80);
    CHECK(elapsed < 2000);
    c.Kernel("NtClose", {event});
  });
}

TEST_CASE("waits: KeDelayExecutionThread honors an absolute deadline", "[char][waits]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto start = std::chrono::steady_clock::now();
    CHECK(c.Kernel("KeDelayExecutionThread",
                   {1, 0, c.Quad(static_cast<i64>(SystemTime(c)) + 100 * kTicksPerMs)}) == 0);
    i64 elapsed = ElapsedMs(start);
    CHECK(elapsed >= 80);
    CHECK(elapsed < 2000);
    start = std::chrono::steady_clock::now();
    CHECK(c.Kernel("KeDelayExecutionThread",
                   {1, 0, c.Quad(static_cast<i64>(SystemTime(c)) - 100 * kTicksPerMs)}) == 0);
    CHECK(ElapsedMs(start) < 500);
  });
}

TEST_CASE("waits: NetDll_WSAWaitForMultipleEvents waits its millisecond timeout",
          "[char][waits][net]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 unsignaled = Event(c, 0);
    const u32 signaled = Event(c, 1);
    const u32 events = c.Alloc(4);
    c.Store32(events, unsignaled);
    const auto start = std::chrono::steady_clock::now();
    CHECK(c.XAM("NetDll_WSAWaitForMultipleEvents", {1, events, 0, 100, 0}) == kTimeout);
    const i64 elapsed = ElapsedMs(start);
    CHECK(elapsed >= 80);
    CHECK(elapsed < 2000);
    c.Store32(events, signaled);
    CHECK(c.XAM("NetDll_WSAWaitForMultipleEvents", {1, events, 0, 100, 0}) == 0);
    c.Kernel("NtClose", {unsignaled});
    c.Kernel("NtClose", {signaled});
  });
}
