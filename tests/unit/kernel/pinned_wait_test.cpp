/**
 * @file        tests/unit/kernel/pinned_wait_test.cpp
 * @brief       Pin and wait functions on real events
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

#include <rex/kernel/title_runtime.h>
#include <rex/kernel/types/status.h>
#include <rex/kernel/object/event_object.h>
#include <rex/kernel/object/symbolic_link_object.h>
#include <rex/kernel/object/object.h>
#include <rex/kernel/object/object_table.h>

#include <rex/kernel/thread_registry.h>
#include <rex/kernel/object/object_pin.h>

#include "kernel_fixture.h"

using rex::kernel::AddOptions;
using rex::kernel::EventObject;
using rex::kernel::SymbolicLinkObject;
using rex::kernel::ToXStatus;
using rex::kernel::X_HANDLE;
using rex::kernel::X_STATUS;
using rex::test::Caller;
using rex::test::KernelFixture;

REX_TEST_CASE("WaitGuestObject returns invalid handle for a missing handle", "[kernel][wait]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    CHECK(c.GetRuntime().GetThreadRegistry().GetObjectWaiter().WaitGuestObject(
              0xF8001230, 3, 1, 0, nullptr) == rex::kernel::X_STATUS_INVALID_HANDLE);
  });
}

REX_TEST_CASE("Wait on a type without a wait handle succeeds at once", "[kernel][wait]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto& table = c.GetRuntime().GetObjectTable();
    auto added = table.Emplace<SymbolicLinkObject>(AddOptions{}, "a", "b");
    CHECKED_ELSE(added.has_value()) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const rex::kernel::ObjectPin object = table.Get(*added);
    REQUIRE(object);
    CHECK(rex::kernel::Wait(c.GetRuntime(), object, 3, 1, 0, nullptr) ==
          rex::kernel::X_STATUS_SUCCESS);
    CHECK(table.Close(*added));
  });
}

REX_TEST_CASE("Wait keeps the object alive when its handle closes mid wait",
              "[kernel][wait][pin]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto& table = c.GetRuntime().GetObjectTable();
    auto added = table.Emplace<EventObject>(AddOptions{}, true, false);
    CHECKED_ELSE(added.has_value()) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const rex::kernel::X_HANDLE handle = *added;
    auto result = std::make_shared<std::promise<u32>>();
    auto done = result->get_future();
    auto started = std::make_shared<std::promise<void>>();
    auto running = started->get_future();
    const u32 waiter = c.Spawn([handle, result, started](Caller& w) {
      const rex::kernel::ObjectPin object = w.GetRuntime().GetObjectTable().Get(handle);
      started->set_value();
      // 2 s relative, well past the close 100 ms in.
      u64 timeout = static_cast<u64>(-20000000);
      result->set_value(rex::kernel::Wait(w.GetRuntime(), object, 3, 1, 0, &timeout));
    });
    running.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(ToXStatus(table.Close(handle)) == rex::kernel::X_STATUS_SUCCESS);
    CHECK(done.wait_for(std::chrono::seconds(20)) == std::future_status::ready);
    CHECK(done.get() == rex::kernel::X_STATUS_TIMEOUT);
    c.Kernel("NtClose", {waiter});
  });
}
