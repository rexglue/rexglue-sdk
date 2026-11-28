/**
 * @file        tests/unit/kernel/identity_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include <rex/kernel/title_runtime.h>
#include <rex/kernel/types/status.h>
#include <rex/kernel/object/object_table.h>

#include "kernel_fixture.h"

using rex::kernel::X_STATUS;

using rex::test::Caller;
using rex::test::KernelFixture;

namespace {
u32 Poll(Caller& c) {
  return c.AllocQuad(0);
}
}  // namespace

REX_TEST_CASE("identity: a guest initialised event keeps its object and takes no handle",
              "[kernel][identity]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 event = c.Alloc(0x10);
    c.Kernel("KeInitializeEvent", {event, 0, 0});
    auto& table = c.GetRuntime().GetObjectTable();
    const auto registered = table.GetByAddress(event);
    REQUIRE(registered);
    CHECK(registered.GetKernelObject().handles.empty());
    CHECK(c.Kernel("KeSetEvent", {event, 0, 0}) == 1);
    const auto again = table.GetByAddress(event);
    REQUIRE(again);
    CHECK(&again.GetKernelObject() == &registered.GetKernelObject());
    CHECK(c.Kernel("KeWaitForSingleObject", {event, 0, 0, 0, Poll(c)}) == 0);
    CHECK(registered.GetKernelObject().handles.empty());
  });
}

REX_TEST_CASE("identity: threads that resolve one new guest event together get one object",
              "[kernel][identity]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr int kThreads = 8;
    constexpr int kCalls = 100;
    const u32 event = c.Alloc(0x10);
    c.Store32(event, 0x01000000);
    c.Store32(event + 4, 0);
    auto go = std::make_shared<std::atomic<bool>>(false);
    std::vector<u32> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
      threads.push_back(c.Spawn([event, go](Caller& t) {
        while (!go->load()) {
          std::this_thread::yield();
        }
        for (int n = 0; n < kCalls; ++n) {
          t.Kernel("KeSetEvent", {event, 0, 0});
        }
      }));
    }
    go->store(true);
    for (const u32 thread : threads) {
      CHECK(c.Kernel("NtWaitForSingleObjectEx", {thread, 1, 0, c.AllocQuad(-100000000)}) == 0);
      c.Kernel("NtClose", {thread});
    }
    int count = 0;
    for (const auto& object : c.GetRuntime().GetObjectTable().GetAll()) {
      if (object.GetKernelObject().guest_ptr == event) {
        ++count;
      }
    }
    CHECK(count == 1);
  });
}

REX_TEST_CASE("identity: a block initialized again while another thread uses it stays one object",
              "[kernel][identity]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr int kRounds = 3000;
    const u32 event = c.Alloc(0x10);
    c.Kernel("KeInitializeEvent", {event, 0, 0});
    auto done = std::make_shared<std::atomic<bool>>(false);
    const u32 user = c.Spawn([event, done](Caller& t) {
      while (!done->load()) {
        t.Kernel("KeSetEvent", {event, 0, 0});
        t.Kernel("KeResetEvent", {event});
      }
    });
    for (int n = 0; n < kRounds; ++n) {
      c.Kernel("KeInitializeEvent", {event, 0, 0});
    }
    done->store(true);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {user, 1, 0, c.AllocQuad(-100000000)}) == 0);
    c.Kernel("NtClose", {user});
    c.Kernel("KeInitializeEvent", {event, 0, 0});
    CHECK(c.Kernel("KeSetEvent", {event, 0, 0}) == 1);
    CHECK(c.Kernel("KeWaitForSingleObject", {event, 0, 0, 0, Poll(c)}) == 0);
    int count = 0;
    for (const auto& object : c.GetRuntime().GetObjectTable().GetAll()) {
      if (object.GetKernelObject().guest_ptr == event) {
        ++count;
      }
    }
    CHECK(count == 1);
  });
}

REX_TEST_CASE(
    "identity: ObReferenceObjectByHandle returns the same guest pointer for the same object",
    "[kernel][identity]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 first = c.Alloc(4);
    const u32 second = c.Alloc(4);
    CHECKED_ELSE(c.Kernel("NtCreateEvent", {handle_ptr, 0, 0, 0}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    CHECKED_ELSE(c.Kernel("ObReferenceObjectByHandle", {handle, 0, first}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    CHECKED_ELSE(c.Kernel("ObReferenceObjectByHandle", {handle, 0, second}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    CHECK(c.Load32(first) == c.Load32(second));
    c.Kernel("ObDereferenceObject", {c.Load32(first)});
    c.Kernel("ObDereferenceObject", {c.Load32(second)});
    c.Kernel("NtClose", {handle});
  });
}

REX_TEST_CASE("identity: the current thread pseudo handle", "[kernel][identity]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 copy_ptr = c.Alloc(4);
    CHECK(c.Kernel("NtDuplicateObject", {0xFFFFFFFE, copy_ptr, 0}) == 0);
    const u32 copy = c.Load32(copy_ptr);
    CHECK(copy >= 0xF8000004);
    const u32 object_ptr = c.Alloc(4);
    CHECK(c.Kernel("ObReferenceObjectByHandle", {0xFFFFFFFE, 0, object_ptr}) == 0);
    CHECK(c.Load32(object_ptr) != 0);
    c.Kernel("ObDereferenceObject", {c.Load32(object_ptr)});
    CHECK(c.Kernel("NtClose", {copy}) == 0);
  });
}
