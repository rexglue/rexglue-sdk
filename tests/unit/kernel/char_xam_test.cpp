/**
 * @file        tests/unit/kernel/char_xam_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/runtime.h>
#include <rex/runtime/content_manager.h>
#include <rex/runtime/object_table.h>
#include <rex/runtime/thread_state.h>
#include <rex/runtime/xam/user_profile.h>
#include <rex/runtime/xmemory.h>
#include <rex/runtime/xtypes.h>

#include "kernel_fixture.h"

using rex::testing::Caller;
using rex::testing::KernelFixture;

REX_TEST_CASE("xam: a content resolve reports the content as not found", "[char][xam]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 buffer = c.Alloc(260);
    CHECK(c.XAM("XamContentResolve", {0, c.Alloc(0x134), buffer, 260, 0, 0, 0}) ==
          rex::X_ERROR_NOT_FOUND);
  });
}

REX_TEST_CASE("xam: a launch with no name ends the title", "[char][xam]") {
  KernelFixture fixture;
  rex::runtime::ThreadState host_state(0, 0, 0, fixture.GetRuntime().GetMemory(),
                                       &fixture.GetRuntime());
  rex::runtime::ThreadState* previous = rex::runtime::ThreadState::Get();
  rex::runtime::ThreadState::Bind(&host_state);
  Caller c(*host_state.GetContext(), fixture.GetRuntime().GetMemory()->GetVirtualMembase(),
           fixture.GetRuntime());
  c.XAM("XamLoaderLaunchTitle", {0, 0});
  rex::runtime::ThreadState::Bind(previous);
  CHECK(fixture.GetRuntime().IsTerminatingTitle());
}

namespace {
constexpr u32 kKnownSetting = 0x10040003;
constexpr u32 kUnknownSetting = 0x10040FFF;
constexpr u32 kBinarySetting = 0x63E80100;
constexpr u32 kHeaderSize = 8;
constexpr u32 kEntrySize = 40;

u32 ReadProfile(Caller& c, u32 user_index, u32 xuid_count, u32 xuids, u32 setting_count,
                u32 setting_ids, u32 buffer) {
  const u32 size_ptr = c.Alloc(4);
  c.Store32(size_ptr, 0);
  c.XAM("XamUserReadProfileSettings",
        {0, user_index, xuid_count, xuids, setting_count, setting_ids, size_ptr, 0, 0});
  return c.XAM("XamUserReadProfileSettings",
               {0, user_index, xuid_count, xuids, setting_count, setting_ids, size_ptr, buffer, 0});
}
}  // namespace

REX_TEST_CASE("xam: a profile read writes 1 entry per XUID and setting", "[char][xam]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u64 own = c.GetRuntime().GetContentManager().GetUserProfile().GetXUID();
    const u64 other = own + 1;
    const u32 xuids = c.Alloc(16);
    c.Store64(xuids, own);
    c.Store64(xuids + 8, other);
    const u32 setting_ids = c.Alloc(4);
    c.Store32(setting_ids, kKnownSetting);
    const u32 buffer = c.Alloc(0x200);
    REQUIRE(ReadProfile(c, 0, 2, xuids, 1, setting_ids, buffer) == 0);
    CHECK(c.Load32(buffer) == 2);
    const u32 first = buffer + kHeaderSize;
    const u32 second = first + kEntrySize;
    CHECK(c.Load32(first) != 0);
    CHECK(c.Load32(first + 12) == static_cast<u32>(own));
    CHECK(c.Load32(first + 32) == 3);
    CHECK(c.Load32(second) == 0);
    CHECK(c.Load32(second + 12) == static_cast<u32>(other));
    CHECK(c.Load32(second + 16) == kKnownSetting);
    CHECK(c.Load32(second + 32) == 0);
  });
}

REX_TEST_CASE("xam: an unknown profile setting reads as no value", "[char][xam]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 setting_ids = c.Alloc(8);
    c.Store32(setting_ids, kKnownSetting);
    c.Store32(setting_ids + 4, kUnknownSetting);
    const u32 buffer = c.Alloc(0x200);
    REQUIRE(ReadProfile(c, 0, 0, 0, 2, setting_ids, buffer) == 0);
    CHECK(c.Load32(buffer) == 2);
    const u32 second = buffer + kHeaderSize + kEntrySize;
    CHECK(c.Load32(buffer + kHeaderSize) != 0);
    CHECK(c.Load32(second) == 0);
    CHECK(c.Load32(second + 16) == kUnknownSetting);
  });
}

namespace {
u32 CreateSession(Caller& c) {
  const u32 handle_ptr = c.Alloc(4);
  REQUIRE(c.XAM("XamSessionCreateHandle", {handle_ptr}) == 0);
  return c.Load32(handle_ptr);
}

bool HandleResolves(Caller& c, u32 handle) {
  return static_cast<bool>(c.GetRuntime().GetObjectTable().Get(handle));
}
}  // namespace

REX_TEST_CASE("xam: a session reference holds the session until its dereference", "[char][xam]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 session = CreateSession(c);
    const u32 object_ptr = c.Alloc(4);
    REQUIRE(c.XAM("XamSessionRefObjByHandle", {session, object_ptr}) == 0);
    const u32 object = c.Load32(object_ptr);
    CHECK(object != 0);
    CHECK(c.Kernel("NtClose", {session}) == 0);
    CHECK(HandleResolves(c, session));
    c.Kernel("ObDereferenceObject", {object});
    CHECK_FALSE(HandleResolves(c, session));
  });
}

REX_TEST_CASE("xam: a session dereference and close release the session", "[char][xam]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 session = CreateSession(c);
    const u32 object_ptr = c.Alloc(4);
    REQUIRE(c.XAM("XamSessionRefObjByHandle", {session, object_ptr}) == 0);
    c.Kernel("ObDereferenceObject", {c.Load32(object_ptr)});
    CHECK(HandleResolves(c, session));
    CHECK(c.Kernel("NtClose", {session}) == 0);
    CHECK_FALSE(HandleResolves(c, session));
  });
}

REX_TEST_CASE("xam: a session reference refuses a handle that is not a session", "[char][xam]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 object_ptr = c.Alloc(4);
    CHECK(c.XAM("XamSessionRefObjByHandle", {0xF8001234, object_ptr}) ==
          rex::X_ERROR_INVALID_HANDLE);
    const u32 event_ptr = c.Alloc(4);
    REQUIRE(c.Kernel("NtCreateEvent", {event_ptr, 0, 0, 0}) == 0);
    const u32 event = c.Load32(event_ptr);
    CHECK(c.XAM("XamSessionRefObjByHandle", {event, object_ptr}) == rex::X_ERROR_INVALID_HANDLE);
    CHECK(c.Kernel("NtClose", {event}) == 0);
  });
}

REX_TEST_CASE("xam: a binary profile setting of size 0 is stored with no bytes", "[kernel][xam]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kBinaryType = 6;
    const u32 entry = c.Alloc(kEntrySize);
    c.Store32(entry + 16, kBinarySetting);
    c.Store32(entry + 24, kBinaryType << 24);
    c.Store32(entry + 32, 0);
    c.Store32(entry + 36, c.Alloc(4));
    REQUIRE(c.XAM("XamUserWriteProfileSettings", {0, 0, 1, entry, 0}) == 0);

    const u32 setting_ids = c.Alloc(4);
    c.Store32(setting_ids, kBinarySetting);
    const u32 buffer = c.Alloc(0x200);
    REQUIRE(ReadProfile(c, 0, 0, 0, 1, setting_ids, buffer) == 0);
    const u32 first = buffer + kHeaderSize;
    CHECK(c.Load8(first + 24) == kBinaryType);
    CHECK(c.Load32(first + 32) == 0);
  });
}
