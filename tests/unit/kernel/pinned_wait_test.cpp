/**
 * @file        tests/unit/kernel/pinned_wait_test.cpp
 * @brief       Tests for the pin and wait functions on real events
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <thread>

#include <rex/system/kernel_state.h>
#include <rex/system/xobject.h>

#include "kernel_fixture.h"

using rex::X_HANDLE;
using rex::X_STATUS;
using rex::system::AddOptions;
using rex::system::EventObject;
using rex::system::SymbolicLinkObject;
using rex::testing::Caller;
using rex::testing::KernelFixture;

TEST_CASE("WaitOn returns invalid handle for a missing handle", "[wait]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto* ks = c.runtime().kernel_state();
    CHECK(rex::system::WaitOn(ks, 0xF8001230, 3, 1, 0, nullptr) == X_STATUS_INVALID_HANDLE);
  });
}

TEST_CASE("WaitOn on a kind without a wait handle succeeds at once", "[wait]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto* ks = c.runtime().kernel_state();
    X_HANDLE handle = 0;
    REQUIRE(ks->object_table()->Emplace<SymbolicLinkObject>(&handle, AddOptions{}, "a", "b") ==
            X_STATUS_SUCCESS);
    CHECK(rex::system::WaitOn(ks, handle, 3, 1, 0, nullptr) == X_STATUS_SUCCESS);
    ks->object_table()->Close(handle);
  });
}

TEST_CASE("WaitOn keeps the object alive when its handle closes mid wait", "[wait][pin]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto* ks = c.runtime().kernel_state();
    X_HANDLE handle = 0;
    REQUIRE(ks->object_table()->Emplace<EventObject>(&handle, AddOptions{}, true, false) ==
            X_STATUS_SUCCESS);
    auto result = std::make_shared<std::promise<u32>>();
    auto done = result->get_future();
    auto started = std::make_shared<std::promise<void>>();
    auto running = started->get_future();
    const u32 waiter = c.Spawn([ks, handle, result, started](Caller&) {
      started->set_value();
      u64 timeout = static_cast<u64>(-100000000);
      result->set_value(rex::system::WaitOn(ks, handle, 3, 1, 0, &timeout));
    });
    running.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(ks->object_table()->Close(handle) == X_STATUS_SUCCESS);
    CHECK(done.wait_for(std::chrono::seconds(20)) == std::future_status::ready);
    CHECK(done.get() == X_STATUS_TIMEOUT);
    c.Kernel("NtClose", {waiter});
  });
}
