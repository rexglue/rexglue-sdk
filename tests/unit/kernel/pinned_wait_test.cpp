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

#include "rex_test.h"

#include <chrono>
#include <future>
#include <memory>
#include <thread>

#include <rex/runtime.h>
#include <rex/runtime/error.h>
#include <rex/runtime/xobject.h>

#include "kernel_fixture.h"

using rex::X_HANDLE;
using rex::X_STATUS;
using rex::runtime::AddOptions;
using rex::runtime::EventObject;
using rex::runtime::SymbolicLinkObject;
using rex::runtime::ToXStatus;
using rex::testing::Caller;
using rex::testing::KernelFixture;

REX_TEST_CASE("WaitOn returns invalid handle for a missing handle", "[wait]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    CHECK(rex::runtime::WaitOn(c.runtime(), 0xF8001230, 3, 1, 0, nullptr) ==
          rex::X_STATUS_INVALID_HANDLE);
  });
}

REX_TEST_CASE("WaitOn on a kind without a wait handle succeeds at once", "[wait]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto& table = c.runtime().object_table();
    auto added = table.Emplace<SymbolicLinkObject>(AddOptions{}, "a", "b");
    CHECKED_ELSE(added.has_value()) {
      FAIL_CHECK("the check above failed");
      return;
    }
    CHECK(rex::runtime::WaitOn(c.runtime(), *added, 3, 1, 0, nullptr) == rex::X_STATUS_SUCCESS);
    CHECK(table.Close(*added));
  });
}

REX_TEST_CASE("WaitOn keeps the object alive when its handle closes mid wait", "[wait][pin]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto& table = c.runtime().object_table();
    auto added = table.Emplace<EventObject>(AddOptions{}, true, false);
    CHECKED_ELSE(added.has_value()) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const X_HANDLE handle = *added;
    auto result = std::make_shared<std::promise<u32>>();
    auto done = result->get_future();
    auto started = std::make_shared<std::promise<void>>();
    auto running = started->get_future();
    const u32 waiter = c.Spawn([handle, result, started](Caller& w) {
      started->set_value();
      u64 timeout = static_cast<u64>(-100000000);
      result->set_value(rex::runtime::WaitOn(w.runtime(), handle, 3, 1, 0, &timeout));
    });
    running.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(ToXStatus(table.Close(handle)) == rex::X_STATUS_SUCCESS);
    CHECK(done.wait_for(std::chrono::seconds(20)) == std::future_status::ready);
    CHECK(done.get() == rex::X_STATUS_TIMEOUT);
    c.Kernel("NtClose", {waiter});
  });
}
