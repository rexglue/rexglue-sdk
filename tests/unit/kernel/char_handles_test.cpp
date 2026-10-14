/**
 * @file        tests/unit/kernel/char_handles_test.cpp
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
#include <rex/runtime/error.h>
#include <rex/runtime/object_table.h>
#include <rex/runtime/object.h>
#include <rex/runtime/xtypes.h>

#include "kernel/object_reference.h"
#include "kernel_fixture.h"

using rex::X_STATUS;

using rex::testing::Caller;
using rex::testing::KernelFixture;

namespace {
constexpr u32 kInvalidHandle = rex::X_STATUS_INVALID_HANDLE;
constexpr u32 kTimeout = rex::X_STATUS_TIMEOUT;
u32 Poll(Caller& c) {
  return c.Quad(0);
}
}  // namespace

REX_TEST_CASE("handles: duplicate close and reuse", "[char][handles]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 copy_ptr = c.Alloc(4);
    CHECKED_ELSE(c.Kernel("NtCreateEvent", {handle_ptr, 0, 0, 0}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    CHECKED_ELSE(c.Kernel("NtDuplicateObject", {handle, copy_ptr, 0}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 copy = c.Load32(copy_ptr);
    CHECK(copy != handle);
    CHECK(c.Kernel("NtSetEvent", {copy, 0}) == 0);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, Poll(c)}) == 0);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
    CHECK(c.Kernel("NtSetEvent", {handle, 0}) == kInvalidHandle);
    CHECK(c.Kernel("NtSetEvent", {copy, 0}) == 0);
    CHECK(c.Kernel("NtClose", {copy}) == 0);
    CHECKED_ELSE(c.Kernel("NtCreateEvent", {handle_ptr, 0, 0, 0}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 reused = c.Load32(handle_ptr);
    CHECK(reused >= 0xF8000004);
    CHECK((reused & 3) == 0);
    c.Kernel("NtClose", {reused});
  });
}

REX_TEST_CASE("handles: close of pseudo and bad handles returns a status", "[char][handles]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    CHECK(c.Kernel("NtClose", {0}) == kInvalidHandle);
    CHECK(c.Kernel("NtClose", {0xFFFFFFFF}) == kInvalidHandle);
    CHECK(c.Kernel("NtClose", {0xF8FFFFF0}) == kInvalidHandle);
    CHECK(c.Kernel("NtClose", {0x12345678}) == kInvalidHandle);
  });
}

REX_TEST_CASE("handles: ObReferenceObjectByHandle and ObDereferenceObject keep the object open",
              "[char][handles]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 object_ptr = c.Alloc(4);
    CHECKED_ELSE(c.Kernel("NtCreateEvent", {handle_ptr, 0, 0, 1}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    CHECKED_ELSE(c.Kernel("ObReferenceObjectByHandle", {handle, 0, object_ptr}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 object = c.Load32(object_ptr);
    CHECK(object != 0);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
    CHECK(c.Kernel("NtSetEvent", {handle, 0}) == 0);
    CHECK(c.Kernel("ObDereferenceObject", {object}) == 0);
    CHECK(c.Kernel("NtSetEvent", {handle, 0}) == kInvalidHandle);
  });
}

REX_TEST_CASE("handles: ObDereferenceObject and ObOpenObjectByPointer reject a null object",
              "[char][handles]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    CHECK(c.Kernel("ObDereferenceObject", {0}) == 0);
    CHECK(c.Kernel("ObOpenObjectByPointer", {0, handle_ptr}) != 0);
  });
}

REX_TEST_CASE("handles: ObOpenObjectByPointer returns a closable handle", "[char][handles]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 event = c.Alloc(0x10);
    c.Kernel("KeInitializeEvent", {event, 0, 0});
    const u32 handle_ptr = c.Alloc(4);
    CHECKED_ELSE(c.Kernel("ObOpenObjectByPointer", {event, handle_ptr}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    CHECK(c.Kernel("NtSetEvent", {handle, 0}) == 0);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
    CHECK(c.Kernel("NtSetEvent", {handle, 0}) == 0);
  });
}

REX_TEST_CASE("names: case insensitive open and collision", "[char][names]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 again_ptr = c.Alloc(4);
    CHECKED_ELSE(c.Kernel("NtCreateEvent", {handle_ptr, c.Attributes("TestEvent"), 0, 0}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    CHECK(c.Kernel("NtCreateEvent", {again_ptr, c.Attributes("testevent"), 0, 0}) ==
          rex::X_STATUS_OBJECT_NAME_EXISTS);
    CHECK(c.Load32(again_ptr) == handle);
    CHECK(c.Kernel("ObOpenObjectByName", {c.Attributes("TESTEVENT"), 0, 0, again_ptr}) == 0);
    CHECK(c.Load32(again_ptr) == handle);
    c.Kernel("NtClose", {handle});
    c.Kernel("NtClose", {handle});
    c.Kernel("NtClose", {handle});
    CHECK(c.Kernel("ObOpenObjectByName", {c.Attributes("TestEvent"), 0, 0, again_ptr}) ==
          rex::X_STATUS_OBJECT_NAME_NOT_FOUND);
  });
}

REX_TEST_CASE("names: another kind holding the name", "[char][names]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 other_ptr = c.Alloc(4);
    CHECKED_ELSE(c.Kernel("NtCreateSemaphore", {handle_ptr, c.Attributes("Shared"), 1, 2}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 semaphore = c.Load32(handle_ptr);
    CHECK(c.Kernel("NtCreateEvent", {other_ptr, c.Attributes("Shared"), 0, 0}) == 0);
    const u32 event = c.Load32(other_ptr);
    CHECK(event != semaphore);
    CHECK(c.Kernel("ObOpenObjectByName", {c.Attributes("Shared"), 0, 0, other_ptr}) == 0);
    CHECK(c.Load32(other_ptr) == semaphore);
    c.Kernel("NtClose", {event});
    c.Kernel("NtClose", {semaphore});
  });
}

REX_TEST_CASE("waits: wait any and wait all", "[char][waits]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 handles = c.Alloc(8);
    CHECKED_ELSE(c.Kernel("NtCreateEvent", {handle_ptr, 0, 0, 0}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 first = c.Load32(handle_ptr);
    CHECKED_ELSE(c.Kernel("NtCreateEvent", {handle_ptr, 0, 0, 0}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 second = c.Load32(handle_ptr);
    c.Store32(handles, first);
    c.Store32(handles + 4, second);
    CHECK(c.Kernel("NtWaitForMultipleObjectsEx", {2, handles, 1, 1, 0, Poll(c)}) == kTimeout);
    c.Kernel("NtSetEvent", {second, 0});
    CHECK(c.Kernel("NtWaitForMultipleObjectsEx", {2, handles, 1, 1, 0, Poll(c)}) == 1);
    CHECK(c.Kernel("NtWaitForMultipleObjectsEx", {2, handles, 0, 1, 0, Poll(c)}) == kTimeout);
    c.Kernel("NtSetEvent", {first, 0});
    CHECK(c.Kernel("NtWaitForMultipleObjectsEx", {2, handles, 0, 1, 0, Poll(c)}) == 0);
    c.Store32(handles + 4, 0xF8001230);
    CHECK(c.Kernel("NtWaitForMultipleObjectsEx", {2, handles, 0, 1, 0, Poll(c)}) ==
          rex::X_STATUS_INVALID_PARAMETER);
    c.Kernel("NtClose", {first});
    c.Kernel("NtClose", {second});
  });
}

REX_TEST_CASE("waits: signal and wait", "[char][waits]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    CHECKED_ELSE(c.Kernel("NtCreateEvent", {handle_ptr, 0, 1, 0}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 signal = c.Load32(handle_ptr);
    CHECKED_ELSE(c.Kernel("NtCreateEvent", {handle_ptr, 0, 1, 0}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 wait = c.Load32(handle_ptr);
    CHECK(c.Kernel("NtSignalAndWaitForSingleObjectEx", {signal, wait, 0, 0, Poll(c)}) == kTimeout);
    CHECK(c.Kernel("NtSignalAndWaitForSingleObjectEx", {wait, signal, 0, 0, Poll(c)}) == 0);
    CHECK(c.Kernel("NtSignalAndWaitForSingleObjectEx", {0xF8001230, wait, 0, 0, Poll(c)}) ==
          kInvalidHandle);
    c.Kernel("NtClose", {signal});
    c.Kernel("NtClose", {wait});
  });
}

REX_TEST_CASE("waits: a handle closed during a wait still completes the wait",
              "[char][waits][pin]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    CHECKED_ELSE(c.Kernel("NtCreateEvent", {handle_ptr, 0, 0, 0}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    auto result = std::make_shared<std::promise<u32>>();
    auto done = result->get_future();
    const u32 waiter = c.Spawn([handle, result](Caller& w) {
      result->set_value(w.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, w.Quad(-20000000)}));
    });
    c.Kernel("NtClose", {handle});
    CHECKED_ELSE(done.wait_for(std::chrono::seconds(10)) == std::future_status::ready) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 outcome = done.get();
    CHECK((outcome == kTimeout || outcome == kInvalidHandle));
    c.Kernel("NtClose", {waiter});
  });
}

REX_TEST_CASE("handles: a lookup whose object lost its last handle takes no reference",
              "[char][handles][pin]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto& table = c.GetRuntime().GetObjectTable();
    auto added =
        table.Emplace<rex::runtime::SymbolicLinkObject>(rex::runtime::AddOptions{}, "a", "b");
    if (!added) {
      FAIL("the symbolic link was not added");
      return;
    }
    const auto object = table.Get(*added);
    REQUIRE(object);
    REQUIRE(table.Close(*added));

    auto opened = rex::kernel::OpenPrimaryHandle(table, object);
    CHECK(rex::runtime::ToXStatus(opened) == kInvalidHandle);
    auto referenced = rex::kernel::ReferencePrimary(table, object, 0x1234);
    CHECK(rex::runtime::ToXStatus(referenced) == kInvalidHandle);
    auto thread = rex::kernel::ReferenceThread(table, *added, 0x1234);
    CHECK(rex::runtime::ToXStatus(thread) == kInvalidHandle);
  });
}
