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

#include <catch2/catch_test_macros.hpp>

#include <cstring>

#include <rex/runtime.h>
#include <rex/runtime/xenumerator.h>
#include <rex/runtime/xmemory.h>
#include <rex/runtime/xobject.h>
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

TEST_CASE("enumerate: a buffer size smaller than 1 item still takes the full batch",
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

TEST_CASE("enumerate: the aggregate message takes an item with a buffer size below 1 item",
          "[char][enumerate]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    rex::runtime::StaticItems items(1, kItemSize);
    std::memset(items.Append(), 0xAB, kItemSize);
    std::memset(items.Append(), 0xCD, kItemSize);
    rex::X_HANDLE handle = 0;
    void* extra_buffer = nullptr;
    const auto status = rex::runtime::CreateEnumerator(
        c.runtime(), &handle, {0xFF, kXAMApp, kEnumerateAggregate, 0x00020010, 0},
        rex::runtime::EnumeratorObject(
            1, kItemSize,
            [items = std::move(items)](u32, u8* buffer_data, u32* written_count) mutable {
              return items.Write(buffer_data, written_count);
            }),
        sizeof(rex::runtime::X_KENUMERATOR_CONTENT_AGGREGATE), &extra_buffer);
    REQUIRE(status == rex::X_STATUS_SUCCESS);
    auto* extra = static_cast<rex::runtime::X_KENUMERATOR_CONTENT_AGGREGATE*>(extra_buffer);
    extra->magic = rex::runtime::kXObjSignature;
    extra->handle = handle;

    const u32 buffer = c.Alloc(kItemSize);
    const u32 length_ptr = c.Alloc(4);
    const u32 message = c.Alloc(0x20);
    c.Store32(message + 0x08, c.runtime().memory()->HostToGuestVirtual(extra_buffer));
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
