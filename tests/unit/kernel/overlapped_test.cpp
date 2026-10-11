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

#include <chrono>
#include <cstddef>
#include <thread>

#include <rex/runtime.h>
#include <rex/runtime/dispatcher.h>
#include <rex/runtime/xio.h>

#include "kernel_fixture.h"

using rex::runtime::XAM_OVERLAPPED;
using rex::testing::Caller;
using rex::testing::KernelFixture;

REX_TEST_CASE("Complete writes the result, the extended error and the length", "[kernel]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 overlapped = c.Alloc(sizeof(XAM_OVERLAPPED));
    c.runtime().dispatcher().Complete(
        overlapped, {.result = rex::X_ERROR_SUCCESS, .extended_error = 5, .length = 64});
    REQUIRE(c.Load32(overlapped + offsetof(XAM_OVERLAPPED, result)) == rex::X_ERROR_SUCCESS);
    REQUIRE(c.Load32(overlapped + offsetof(XAM_OVERLAPPED, length)) == 64);
    REQUIRE(c.Load32(overlapped + offsetof(XAM_OVERLAPPED, extended_error)) == 5);
  });
}

REX_TEST_CASE("Complete derives the extended error and the length from a failure", "[kernel]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 overlapped = c.Alloc(sizeof(XAM_OVERLAPPED));
    c.runtime().dispatcher().Complete(overlapped, {.result = rex::X_ERROR_NO_SUCH_USER});
    REQUIRE(c.Load32(overlapped + offsetof(XAM_OVERLAPPED, result)) == rex::X_ERROR_NO_SUCH_USER);
    REQUIRE(c.Load32(overlapped + offsetof(XAM_OVERLAPPED, length)) == 0xFFFFFFFF);
    REQUIRE(c.Load32(overlapped + offsetof(XAM_OVERLAPPED, extended_error)) ==
            rex::X_ERROR_NO_SUCH_USER);
  });
}

REX_TEST_CASE("CompleteDeferred writes an explicit length on a failure", "[kernel]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 overlapped = c.Alloc(sizeof(XAM_OVERLAPPED));
    c.runtime().dispatcher().CompleteDeferred(overlapped, [] {
      return rex::runtime::CompletionOptions{.result = rex::X_ERROR_FUNCTION_FAILED, .length = 0};
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (c.Load32(overlapped + offsetof(XAM_OVERLAPPED, result)) == rex::X_ERROR_IO_PENDING &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(c.Load32(overlapped + offsetof(XAM_OVERLAPPED, result)) ==
            rex::X_ERROR_FUNCTION_FAILED);
    REQUIRE(c.Load32(overlapped + offsetof(XAM_OVERLAPPED, length)) == 0);
    REQUIRE(c.Load32(overlapped + offsetof(XAM_OVERLAPPED, extended_error)) ==
            rex::X_ERROR_FUNCTION_FAILED);
  });
}
