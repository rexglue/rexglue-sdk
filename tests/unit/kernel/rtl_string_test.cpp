/**
 * @file        tests/unit/kernel/rtl_string_test.cpp
 * @brief       Rtl string copies and conversions stay inside the buffers the title gives
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <array>
#include <vector>

#include <rex/runtime.h>

#include "kernel_fixture.h"

using rex::testing::Caller;
using rex::testing::Get;
using rex::testing::KernelFixture;
using rex::testing::Put;
using rex::testing::PutFilled;

namespace {

constexpr u32 kBufferOverflow = 0x80000005;
constexpr u32 kGuardBytes = 24;
constexpr std::array<u8, 4> kPoison = {0xDE, 0xAD, 0xBE, 0xEF};

u32 MakeString(Caller& c, u32 length, u32 maximum_length, u32 pointer) {
  const u32 address = c.Alloc(8);
  c.Store32(address, (length << 16) | maximum_length);
  c.Store32(address + 4, pointer);
  return address;
}

u32 Length(Caller& c, u32 address) {
  return c.Load32(address) >> 16;
}

void CheckPoison(const std::vector<u8>& bytes, u32 first) {
  for (u32 i = first; i < bytes.size(); ++i) {
    CAPTURE(i);
    CHECK(bytes[i] == kPoison[i % kPoison.size()]);
  }
}

}  // namespace

REX_TEST_CASE("rtl strings: RtlCopyUnicodeString copies the byte length and nothing past it",
              "[kernel][rtl]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const std::vector<u8> text = {0x00, 0x41, 0x00, 0x42, 0x00, 0x43, 0x00, 0x44};
    const u32 source_buffer = Put(c, text);
    const u32 source = MakeString(c, 8, 8, source_buffer);
    const u32 dest_buffer = PutFilled(c, 8 + kGuardBytes);
    const u32 dest = MakeString(c, 0, 8, dest_buffer);
    c.Kernel("RtlCopyUnicodeString", {dest, source});
    CHECK(Length(c, dest) == 8);
    const std::vector<u8> copied = Get(c, dest_buffer, 8 + kGuardBytes);
    CHECK(std::vector<u8>(copied.begin(), copied.begin() + 8) == text);
    CheckPoison(copied, 8);
  });
}

REX_TEST_CASE("rtl strings: RtlCopyUnicodeString stops at the capacity of the destination",
              "[kernel][rtl]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const std::vector<u8> text = {0x00, 0x41, 0x00, 0x42, 0x00, 0x43, 0x00, 0x44};
    const u32 source = MakeString(c, 8, 8, Put(c, text));
    const u32 dest_buffer = PutFilled(c, 4 + kGuardBytes);
    const u32 dest = MakeString(c, 0, 4, dest_buffer);
    c.Kernel("RtlCopyUnicodeString", {dest, source});
    CHECK(Length(c, dest) == 4);
    const std::vector<u8> copied = Get(c, dest_buffer, 4 + kGuardBytes);
    CHECK(std::vector<u8>(copied.begin(), copied.begin() + 4) ==
          std::vector<u8>(text.begin(), text.begin() + 4));
    CheckPoison(copied, 4);
  });
}

REX_TEST_CASE(
    "rtl strings: RtlUnicodeStringToAnsiString writes nothing into a buffer of capacity 0",
    "[kernel][rtl]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 source = MakeString(c, 2, 2, Put(c, std::vector<u8>{0x00, 0x41}));
    const u32 dest_buffer = PutFilled(c, kGuardBytes);
    const u32 dest = MakeString(c, 0, 0, dest_buffer);
    CHECK(c.Kernel("RtlUnicodeStringToAnsiString", {dest, source, 0}) == kBufferOverflow);
    CheckPoison(Get(c, dest_buffer, kGuardBytes), 0);
  });
}

REX_TEST_CASE("rtl strings: RtlUnicodeStringToAnsiString keeps a short buffer terminated",
              "[kernel][rtl]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 source = MakeString(c, 6, 6, Put(c, std::vector<u8>{0, 0x41, 0, 0x42, 0, 0x43}));
    const u32 dest_buffer = PutFilled(c, 2 + kGuardBytes);
    const u32 dest = MakeString(c, 0, 2, dest_buffer);
    CHECK(c.Kernel("RtlUnicodeStringToAnsiString", {dest, source, 0}) == kBufferOverflow);
    const std::vector<u8> bytes = Get(c, dest_buffer, 2 + kGuardBytes);
    CHECK(bytes[0] == 'A');
    CHECK(bytes[1] == 0);
    CheckPoison(bytes, 2);
  });
}

REX_TEST_CASE("rtl strings: RtlMultiByteToUnicodeN accepts a null result size", "[kernel][rtl]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 source = Put(c, std::vector<u8>{'A', 'b'});
    const u32 dest = PutFilled(c, 4 + kGuardBytes);
    CHECK(c.Kernel("RtlMultiByteToUnicodeN", {dest, 4, 0, source, 2}) == 0);
    const std::vector<u8> bytes = Get(c, dest, 4 + kGuardBytes);
    CHECK(std::vector<u8>(bytes.begin(), bytes.begin() + 4) == std::vector<u8>{0, 'A', 0, 'b'});
    CheckPoison(bytes, 4);
  });
}
