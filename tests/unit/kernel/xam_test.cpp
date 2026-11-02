/**
 * @file        tests/unit/kernel/xam_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <cstring>

#include <rex/kernel/types/content.h>
#include <rex/kernel/title_runtime.h>
#include <rex/xam/content_manager.h>
#include <rex/xam/user_profile.h>
#include <rex/kernel/thread_state.h>
#include <rex/kernel/heap.h>
#include <rex/kernel/types/status.h>
#include <rex/kernel/object/object_table.h>

#include "kernel_fixture.h"
#include "kernel/title_state.h"
#include "xam/xam_state.h"

using rex::test::Caller;
using rex::test::KernelFixture;

REX_TEST_CASE("XAM: a content resolve reports the content as not found", "[kernel][xam]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 buffer = c.Alloc(260);
    CHECK(c.XAM("XamContentResolve", {0, c.Alloc(0x134), buffer, 260, 0, 0, 0}) ==
          rex::kernel::X_ERROR_NOT_FOUND);
  });
}

REX_TEST_CASE("XAM: a launch with no name ends the title", "[kernel][xam]") {
  KernelFixture fixture;
  rex::kernel::ThreadState host_state(0, 0, 0, StateOf(fixture.GetRuntime()).GetMemory(),
                                      &fixture.GetRuntime());
  rex::kernel::ThreadState* previous = rex::kernel::ThreadState::Get();
  rex::kernel::ThreadState::Bind(&host_state);
  Caller c(*host_state.GetContext(), StateOf(fixture.GetRuntime()).GetMemory()->GetVirtualMembase(),
           fixture.GetRuntime());
  c.XAM("XamLoaderLaunchTitle", {0, 0});
  rex::kernel::ThreadState::Bind(previous);
  CHECK(StateOf(fixture.GetRuntime()).IsTerminatingTitle());
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

REX_TEST_CASE("XAM: a profile read writes 1 entry per XUID and setting", "[kernel][xam]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u64 own = rex::xam::CurrentXAM().GetUserProfile().GetXUID();
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

REX_TEST_CASE("XAM: an unknown profile setting reads as no value", "[kernel][xam]") {
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

REX_TEST_CASE("XAM: a session reference holds the session until its dereference", "[kernel][xam]") {
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

REX_TEST_CASE("XAM: a session dereference and close release the session", "[kernel][xam]") {
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

REX_TEST_CASE("XAM: a session reference refuses a handle that is not a session", "[kernel][xam]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 object_ptr = c.Alloc(4);
    CHECK(c.XAM("XamSessionRefObjByHandle", {0xF8001234, object_ptr}) ==
          rex::kernel::X_ERROR_INVALID_HANDLE);
    const u32 event_ptr = c.Alloc(4);
    REQUIRE(c.Kernel("NtCreateEvent", {event_ptr, 0, 0, 0}) == 0);
    const u32 event = c.Load32(event_ptr);
    CHECK(c.XAM("XamSessionRefObjByHandle", {event, object_ptr}) ==
          rex::kernel::X_ERROR_INVALID_HANDLE);
    CHECK(c.Kernel("NtClose", {event}) == 0);
  });
}

REX_TEST_CASE("XAM: a binary profile setting of size 0 is stored with no bytes", "[kernel][xam]") {
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

namespace {
constexpr u32 kXAMApp = 0xFE;
constexpr u32 kQueryVolumeDeviceType = 0x00020021;
constexpr u32 kQueryMessageSize = 0x4C;
constexpr u32 kSentinel = 0x5A5A5A5A;

u32 QueryVolumeDeviceType(Caller& c, const char* root, u32 result_ptr) {
  const u32 message = c.Alloc(kQueryMessageSize);
  for (u32 i = 0; root[i] != '\0'; ++i) {
    c.Store32(message + (i & ~3u),
              c.Load32(message + (i & ~3u)) |
                  (static_cast<u32>(static_cast<u8>(root[i])) << (24 - 8 * (i & 3))));
  }
  c.Store32(message + 0x44, result_ptr);
  return c.XAM("XMsgInProcessCall", {kXAMApp, kQueryVolumeDeviceType, message, kQueryMessageSize});
}
}  // namespace

REX_TEST_CASE("XAM: opening content reads the license from the header of the package owner",
              "[kernel][xam]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kHDD = 1;
    constexpr u32 kOtherTitleId = 0x52455831;
    constexpr u32 kLicense = 5;
    auto& manager = c.GetRuntime().GetContentManager();
    const u64 xuid = rex::xam::CurrentXAM().GetUserProfile().GetXUID();
    rex::kernel::XCONTENT_AGGREGATE_DATA data{};
    data.device_id = kHDD;
    data.content_type = rex::kernel::XContentType::kSavedGame;
    std::memcpy(data.file_name_raw, "rexsave", 7);
    data.xuid = xuid;
    data.title_id = kOtherTitleId;
    REQUIRE(manager.CreateContent("save", xuid, data));
    REQUIRE(manager.WriteContentHeaderFile(xuid, data, kLicense));
    REQUIRE(manager.CloseContent("save"));
    auto opened = manager.OpenContent("again", xuid, data);
    REQUIRE(opened);
    CHECK(*opened == kLicense);
  });
}

REX_TEST_CASE("XAM: a new title numbers its content devices from the first again",
              "[kernel][xam]") {
  const auto device_of_first_package = [] {
    std::string device;
    KernelFixture fixture;
    fixture.RunOnGuestThread([&device](Caller& c) {
      constexpr u32 kHDD = 1;
      const u64 xuid = rex::xam::CurrentXAM().GetUserProfile().GetXUID();
      rex::kernel::XCONTENT_AGGREGATE_DATA data{};
      data.device_id = kHDD;
      data.content_type = rex::kernel::XContentType::kSavedGame;
      std::memcpy(data.file_name_raw, "rexsave", 7);
      data.xuid = xuid;
      data.title_id = 0x52455831;
      REQUIRE(c.GetRuntime().GetContentManager().CreateContent("save", xuid, data));
      const auto link = c.GetRuntime().GetFileSystem().FindLink("save:");
      REQUIRE(link.has_value());
      device = *link;
    });
    return device;
  };
  const std::string first = device_of_first_package();
  const std::string second = device_of_first_package();
  CHECK_FALSE(first.empty());
  CHECK(first == second);
}

REX_TEST_CASE("XAM: the volume device type of a content root is the type of its device",
              "[kernel][xam]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kHDD = 1;
    constexpr u32 kTitleId = 0x52455831;
    const u64 xuid = rex::xam::CurrentXAM().GetUserProfile().GetXUID();
    rex::kernel::XCONTENT_AGGREGATE_DATA data{};
    data.device_id = kHDD;
    data.content_type = rex::kernel::XContentType::kSavedGame;
    std::memcpy(data.file_name_raw, "rexsave", 7);
    data.xuid = xuid;
    data.title_id = kTitleId;
    REQUIRE(c.GetRuntime().GetContentManager().CreateContent("save", xuid, data));

    const u32 result = c.Alloc(4);
    c.Store32(result, kSentinel);
    CHECK(QueryVolumeDeviceType(c, "save", result) == rex::kernel::X_E_SUCCESS);
    CHECK(c.Load32(result) == kHDD);

    c.Store32(result, kSentinel);
    CHECK(QueryVolumeDeviceType(c, "game", result) == rex::kernel::X_E_INVALIDARG);
    CHECK(c.Load32(result) == kSentinel);
  });
}
