/**
 * @file        tests/unit/kernel/char_enumerate_test.cpp
 * @brief       Enumeration writes the batch whatever buffer size the title passes
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include <rex/runtime.h>
#include <rex/runtime/enumerator_object.h>
#include <rex/runtime/xio.h>
#include <rex/runtime/xmemory.h>
#include <rex/runtime/object.h>
#include <rex/runtime/xtypes.h>

#include "kernel_fixture.h"

using rex::testing::Caller;
using rex::testing::KernelFixture;

namespace {
constexpr u32 kDeviceCount = 2;
constexpr u32 kXAMApp = 0xFE;
constexpr u32 kEnumerateAggregate = 0x0002000E;
constexpr u32 kItemSize = 0x40;
constexpr u32 kShortBufferSize = 4;
}  // namespace

REX_TEST_CASE("enumerate: a buffer size smaller than 1 item still takes the full batch",
              "[char][enumerate]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 size_ptr = c.Alloc(4);
    const u32 handle_ptr = c.Alloc(4);
    REQUIRE(c.XAM("XamContentCreateDeviceEnumerator", {0, 0, kDeviceCount, size_ptr, handle_ptr}) ==
            0);
    const u32 handle = c.Load32(handle_ptr);
    const u32 items_ptr = c.Alloc(4);
    const u32 buffer = c.Alloc(c.Load32(size_ptr));
    CHECK(c.XAM("XamEnumerate", {handle, 0, buffer, kShortBufferSize, items_ptr, 0}) ==
          rex::X_ERROR_SUCCESS);
    CHECK(c.Load32(items_ptr) == kDeviceCount);
    CHECK(c.XAM("XamEnumerate", {handle, 0, buffer, kShortBufferSize, items_ptr, 0}) ==
          rex::X_ERROR_NO_MORE_FILES);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

REX_TEST_CASE("enumerate: the aggregate message takes an item with a buffer size below 1 item",
              "[char][enumerate]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    rex::runtime::StaticItems items(1, kItemSize);
    std::memset(items.Append(), 0xAB, kItemSize);
    std::memset(items.Append(), 0xCD, kItemSize);
    rex::X_HANDLE handle = 0;
    void* extra_buffer = nullptr;
    const auto status = rex::runtime::CreateEnumerator(
        c.GetRuntime(), &handle, {0xFF, kXAMApp, kEnumerateAggregate, 0x00020010, 0},
        rex::runtime::EnumeratorObject(
            1, kItemSize,
            [items = std::move(items)](u32, u8* buffer_data, u32* written_count) mutable {
              return items.Write(buffer_data, written_count);
            }),
        sizeof(rex::runtime::X_KENUMERATOR_CONTENT_AGGREGATE), &extra_buffer);
    REQUIRE(status == rex::X_STATUS_SUCCESS);
    auto* extra = static_cast<rex::runtime::X_KENUMERATOR_CONTENT_AGGREGATE*>(extra_buffer);
    extra->magic = rex::runtime::kStashSignature;
    extra->handle = handle;

    const u32 buffer = c.Alloc(kItemSize);
    const u32 length_ptr = c.Alloc(4);
    const u32 message = c.Alloc(0x20);
    c.Store32(message + 0x08, c.GetRuntime().GetMemory()->HostToGuestVirtual(extra_buffer));
    c.Store32(message + 0x0C, buffer);
    c.Store32(message + 0x10, kShortBufferSize);
    c.Store32(message + 0x18, length_ptr);
    CHECK(c.XAM("XMsgInProcessCall", {kXAMApp, kEnumerateAggregate, message, 0x20}) ==
          rex::X_E_SUCCESS);
    CHECK(c.Load32(length_ptr) == 1);
    CHECK(c.Load8(buffer) == 0xAB);
    CHECK(c.XAM("XMsgInProcessCall", {kXAMApp, kEnumerateAggregate, message, 0x20}) ==
          rex::X_E_SUCCESS);
    CHECK(c.Load8(buffer) == 0xCD);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

REX_TEST_CASE("enumerate: ObDereferenceObject on an enumerator leaves its body and handle intact",
              "[kernel][enumerate]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 size_ptr = c.Alloc(4);
    const u32 handle_ptr = c.Alloc(4);
    REQUIRE(c.XAM("XamContentCreateDeviceEnumerator", {0, 0, kDeviceCount, size_ptr, handle_ptr}) ==
            0);
    const rex::X_HANDLE handle = c.Load32(handle_ptr);
    auto& table = c.GetRuntime().GetObjectTable();
    auto enumerator = table.Get<rex::runtime::EnumeratorObject>(handle);
    REQUIRE(enumerator);
    const u32 body = enumerator.GetGuestObject();
    REQUIRE(body != 0);
    std::vector<u8> before(sizeof(rex::runtime::X_KENUMERATOR));
    std::memcpy(before.data(), c.GetRuntime().GetMemory()->TranslateVirtual(body), before.size());
    REQUIRE(table.Retain(handle));

    CHECK(table.ResolveHandle(body, -1) == handle);
    CHECK(c.Kernel("ObDereferenceObject", {body}) == 0);

    std::vector<u8> after(before.size());
    std::memcpy(after.data(), c.GetRuntime().GetMemory()->TranslateVirtual(body), after.size());
    CHECK(after == before);
    CHECK(table.Get<rex::runtime::EnumeratorObject>(handle));
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

REX_TEST_CASE("enumerate: the aggregate message writes a count of 0 when no item is left",
              "[kernel][enumerate]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    rex::X_HANDLE handle = 0;
    void* extra_buffer = nullptr;
    const auto status = rex::runtime::CreateEnumerator(
        c.GetRuntime(), &handle, {0xFF, kXAMApp, kEnumerateAggregate, 0x00020010, 0},
        rex::runtime::EnumeratorObject(1, kItemSize,
                                       [](u32, u8*, u32* written_count) {
                                         *written_count = 0;
                                         return rex::X_ERROR_NO_MORE_FILES;
                                       }),
        sizeof(rex::runtime::X_KENUMERATOR_CONTENT_AGGREGATE), &extra_buffer);
    REQUIRE(status == rex::X_STATUS_SUCCESS);
    auto* extra = static_cast<rex::runtime::X_KENUMERATOR_CONTENT_AGGREGATE*>(extra_buffer);
    extra->magic = rex::runtime::kStashSignature;
    extra->handle = handle;

    constexpr u32 kSentinel = 0x5A5A5A5A;
    const u32 buffer = c.Alloc(kItemSize);
    const u32 length_ptr = c.Alloc(4);
    c.Store32(length_ptr, kSentinel);
    const u32 message = c.Alloc(0x20);
    c.Store32(message + 0x08, c.GetRuntime().GetMemory()->HostToGuestVirtual(extra_buffer));
    c.Store32(message + 0x0C, buffer);
    c.Store32(message + 0x10, kItemSize);
    c.Store32(message + 0x18, length_ptr);
    CHECK(c.XAM("XMsgInProcessCall", {kXAMApp, kEnumerateAggregate, message, 0x20}) ==
          rex::X_E_NO_MORE_FILES);
    CHECK(c.Load32(length_ptr) == 0);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

REX_TEST_CASE("enumerate: XamEnumerate with an invalid handle reports 0 items",
              "[kernel][enumerate]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kSentinel = 0x5A5A5A5A;
    const u32 items_ptr = c.Alloc(4);
    c.Store32(items_ptr, kSentinel);
    const u32 buffer = c.Alloc(kItemSize);
    CHECK(c.XAM("XamEnumerate", {0xBADBAD, 0, buffer, kItemSize, items_ptr, 0}) ==
          rex::X_ERROR_INVALID_HANDLE);
    CHECK(c.Load32(items_ptr) == 0);
  });
}

REX_TEST_CASE("enumerate: an overlapped enumeration of an empty enumerator fails with the HRESULT",
              "[kernel][enumerate]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 size_ptr = c.Alloc(4);
    const u32 handle_ptr = c.Alloc(4);
    REQUIRE(c.XAM("XamContentCreateDeviceEnumerator", {0, 0, kDeviceCount, size_ptr, handle_ptr}) ==
            0);
    const u32 handle = c.Load32(handle_ptr);
    const u32 items_ptr = c.Alloc(4);
    const u32 buffer = c.Alloc(c.Load32(size_ptr));
    REQUIRE(c.XAM("XamEnumerate", {handle, 0, buffer, kShortBufferSize, items_ptr, 0}) ==
            rex::X_ERROR_SUCCESS);

    using rex::runtime::XAM_OVERLAPPED;
    const u32 overlapped = c.Alloc(sizeof(XAM_OVERLAPPED));
    CHECK(c.XAM("XamEnumerate", {handle, 0, buffer, kShortBufferSize, 0, overlapped}) ==
          rex::X_ERROR_IO_PENDING);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (c.LoadAcquire32(overlapped + offsetof(XAM_OVERLAPPED, result)) ==
               rex::X_ERROR_IO_PENDING &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(c.LoadAcquire32(overlapped + offsetof(XAM_OVERLAPPED, result)) ==
          rex::X_ERROR_FUNCTION_FAILED);
    CHECK(c.Load32(overlapped + offsetof(XAM_OVERLAPPED, length)) == 0);
    CHECK(c.Load32(overlapped + offsetof(XAM_OVERLAPPED, extended_error)) ==
          rex::X_E_NO_MORE_FILES);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}
