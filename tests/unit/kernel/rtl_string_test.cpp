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
#include <tuple>
#include <utility>
#include <vector>

#include <rex/runtime.h>

#include "kernel_fixture.h"

using rex::test::Caller;
using rex::test::CopyToGuest;
using rex::test::KernelFixture;
using rex::test::PutFilled;
using rex::test::ReadBytes;

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

constexpr u32 kStringPoison = 0xCCCCCCCC;
constexpr u32 kStringPoisonLength = 0xCCCC;
constexpr u32 kWindowBytes = 32;

std::vector<u8> Wide(std::initializer_list<u16> units) {
  std::vector<u8> bytes;
  for (const u16 unit : units) {
    bytes.push_back(static_cast<u8>(unit >> 8));
    bytes.push_back(static_cast<u8>(unit));
  }
  return bytes;
}

u32 Maximum(Caller& c, u32 address) {
  return c.Load32(address) & 0xFFFF;
}

u32 Pointer(Caller& c, u32 address) {
  return c.Load32(address + 4);
}

void CheckWindow(const std::vector<u8>& bytes, const std::vector<u8>& expected) {
  REQUIRE(bytes.size() >= expected.size());
  CHECK(std::vector<u8>(bytes.begin(), bytes.begin() + expected.size()) == expected);
  CheckPoison(bytes, static_cast<u32>(expected.size()));
}

struct ConvertCase {
  std::vector<u8> text;
  u32 length;
  u32 maximum;
  u32 capacity;
  u32 status;
  u32 result_length;
  std::vector<u8> expected;
};

void RunConversion(const char* name, const std::vector<ConvertCase>& cases) {
  KernelFixture fixture;
  fixture.RunOnGuestThread([&](Caller& c) {
    for (size_t index = 0; index < cases.size(); ++index) {
      const ConvertCase& item = cases[index];
      CAPTURE(name, index);
      const u32 source = MakeString(c, item.length, item.maximum, CopyToGuest(c, item.text));
      const u32 buffer = PutFilled(c, kWindowBytes + kGuardBytes);
      const u32 dest = MakeString(c, kStringPoisonLength, item.capacity, buffer);
      CHECK(c.Kernel(name, {dest, source, 0}) == item.status);
      CHECK(Length(c, dest) == item.result_length);
      CHECK(Maximum(c, dest) == item.capacity);
      CHECK(Pointer(c, dest) == buffer);
      CHECK(Length(c, source) == item.length);
      CHECK(Maximum(c, source) == item.maximum);
      CheckWindow(ReadBytes(c, buffer, kWindowBytes + kGuardBytes), item.expected);
    }
  });
}

void RunAllocatedConversion(const char* name, const char* free_name,
                            const std::vector<ConvertCase>& cases) {
  KernelFixture fixture;
  fixture.RunOnGuestThread([&](Caller& c) {
    for (size_t index = 0; index < cases.size(); ++index) {
      const ConvertCase& item = cases[index];
      CAPTURE(name, index);
      const u32 source = MakeString(c, item.length, item.maximum, CopyToGuest(c, item.text));
      const u32 dest = MakeString(c, kStringPoisonLength, kStringPoisonLength, kStringPoison);
      CHECK(c.Kernel(name, {dest, source, 1}) == item.status);
      CHECK(Length(c, dest) == item.result_length);
      CHECK(Maximum(c, dest) == item.capacity);
      const u32 buffer = Pointer(c, dest);
      REQUIRE(buffer != 0);
      REQUIRE(buffer != kStringPoison);
      CHECK(ReadBytes(c, buffer, static_cast<u32>(item.expected.size())) == item.expected);
      CHECK(Length(c, source) == item.length);
      c.Kernel(free_name, {dest});
      CHECK(Pointer(c, dest) == 0);
    }
  });
}

struct ConvertNCase {
  std::vector<u8> text;
  u32 length;
  u32 destination_length;
  bool report;
  u32 written;
  std::vector<u8> expected;
};

void RunConversionN(const char* name, const std::vector<ConvertNCase>& cases) {
  KernelFixture fixture;
  fixture.RunOnGuestThread([&](Caller& c) {
    for (size_t index = 0; index < cases.size(); ++index) {
      const ConvertNCase& item = cases[index];
      CAPTURE(name, index);
      const u32 buffer = PutFilled(c, kWindowBytes + kGuardBytes);
      const u32 written = c.Alloc(4);
      c.Store32(written, kStringPoison);
      CHECK(c.Kernel(name, {buffer, item.destination_length, item.report ? written : 0,
                            CopyToGuest(c, item.text), item.length}) == 0);
      if (item.report) {
        CHECK(c.Load32(written) == item.written);
      }
      CheckWindow(ReadBytes(c, buffer, kWindowBytes + kGuardBytes), item.expected);
    }
  });
}

}  // namespace

REX_TEST_CASE("rtl strings: RtlCopyUnicodeString copies the byte length and nothing past it",
              "[kernel][rtl]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const std::vector<u8> text = {0x00, 0x41, 0x00, 0x42, 0x00, 0x43, 0x00, 0x44};
    const u32 source_buffer = CopyToGuest(c, text);
    const u32 source = MakeString(c, 8, 8, source_buffer);
    const u32 dest_buffer = PutFilled(c, 8 + kGuardBytes);
    const u32 dest = MakeString(c, 0, 8, dest_buffer);
    c.Kernel("RtlCopyUnicodeString", {dest, source});
    CHECK(Length(c, dest) == 8);
    const std::vector<u8> copied = ReadBytes(c, dest_buffer, 8 + kGuardBytes);
    CHECK(std::vector<u8>(copied.begin(), copied.begin() + 8) == text);
    CheckPoison(copied, 8);
  });
}

REX_TEST_CASE("rtl strings: RtlCopyUnicodeString stops at the capacity of the destination",
              "[kernel][rtl]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const std::vector<u8> text = {0x00, 0x41, 0x00, 0x42, 0x00, 0x43, 0x00, 0x44};
    const u32 source = MakeString(c, 8, 8, CopyToGuest(c, text));
    const u32 dest_buffer = PutFilled(c, 4 + kGuardBytes);
    const u32 dest = MakeString(c, 0, 4, dest_buffer);
    c.Kernel("RtlCopyUnicodeString", {dest, source});
    CHECK(Length(c, dest) == 4);
    const std::vector<u8> copied = ReadBytes(c, dest_buffer, 4 + kGuardBytes);
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
    const u32 source = MakeString(c, 2, 2, CopyToGuest(c, std::vector<u8>{0x00, 0x41}));
    const u32 dest_buffer = PutFilled(c, kGuardBytes);
    const u32 dest = MakeString(c, 0, 0, dest_buffer);
    CHECK(c.Kernel("RtlUnicodeStringToAnsiString", {dest, source, 0}) == kBufferOverflow);
    CheckPoison(ReadBytes(c, dest_buffer, kGuardBytes), 0);
  });
}

REX_TEST_CASE("rtl strings: RtlUnicodeStringToAnsiString keeps a short buffer terminated",
              "[kernel][rtl]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 source =
        MakeString(c, 6, 6, CopyToGuest(c, std::vector<u8>{0, 0x41, 0, 0x42, 0, 0x43}));
    const u32 dest_buffer = PutFilled(c, 2 + kGuardBytes);
    const u32 dest = MakeString(c, 0, 2, dest_buffer);
    CHECK(c.Kernel("RtlUnicodeStringToAnsiString", {dest, source, 0}) == kBufferOverflow);
    const std::vector<u8> bytes = ReadBytes(c, dest_buffer, 2 + kGuardBytes);
    CHECK(bytes[0] == 'A');
    CHECK(bytes[1] == 0);
    CheckPoison(bytes, 2);
  });
}

REX_TEST_CASE("rtl strings: RtlMultiByteToUnicodeN accepts a null result size", "[kernel][rtl]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 source = CopyToGuest(c, std::vector<u8>{'A', 'b'});
    const u32 dest = PutFilled(c, 4 + kGuardBytes);
    CHECK(c.Kernel("RtlMultiByteToUnicodeN", {dest, 4, 0, source, 2}) == 0);
    const std::vector<u8> bytes = ReadBytes(c, dest, 4 + kGuardBytes);
    CHECK(std::vector<u8>(bytes.begin(), bytes.begin() + 4) == std::vector<u8>{0, 'A', 0, 'b'});
    CheckPoison(bytes, 4);
  });
}

REX_TEST_CASE("rtl strings: RtlUnicodeStringToAnsiString into a caller buffer matches the console",
              "[kernel][rtl]") {
  const std::vector<u8> t1 = Wide({0x0041, 0x0020, 0x0062, 0x0031});
  const std::vector<u8> t2 = Wide({0x00E9});
  const std::vector<u8> t3 = Wide({0x3042});
  const std::vector<u8> t4 = Wide({0xDC81});
  const std::vector<u8> t5 = Wide({0x0041, 0x00E9, 0x3042, 0xDC81, 0x007A});
  const std::vector<u8> t7 = Wide({0x0041, 0x0000, 0x0042});
  RunConversion("RtlUnicodeStringToAnsiString",
                {
                    {t1, 8, 8, 5, 0, 4, {0x41, 0x20, 0x62, 0x31, 0x00}},
                    {t1, 8, 8, 4, kBufferOverflow, 3, {0x41, 0x20, 0x62, 0x00}},
                    {t1, 8, 8, 1, kBufferOverflow, 0, {0x00}},
                    {t1, 8, 8, 0, kBufferOverflow, 4, {}},
                    {t2, 2, 2, 2, 0, 1, {0xE9, 0x00}},
                    {t2, 2, 2, 1, kBufferOverflow, 0, {0x00}},
                    {t2, 2, 2, 0, kBufferOverflow, 1, {}},
                    {t3, 2, 2, 2, 0, 1, {0x3F, 0x00}},
                    {t3, 2, 2, 1, kBufferOverflow, 0, {0x00}},
                    {t4, 2, 2, 2, 0, 1, {0x3F, 0x00}},
                    {t4, 2, 2, 0, kBufferOverflow, 1, {}},
                    {t5, 10, 10, 6, 0, 5, {0x41, 0xE9, 0x3F, 0x3F, 0x7A, 0x00}},
                    {t5, 10, 10, 5, kBufferOverflow, 4, {0x41, 0xE9, 0x3F, 0x3F, 0x00}},
                    {t5, 10, 10, 1, kBufferOverflow, 0, {0x00}},
                    {t5, 10, 10, 0, kBufferOverflow, 5, {}},
                    {{}, 0, 0, 1, 0, 0, {0x00}},
                    {{}, 0, 0, 0, kBufferOverflow, 0, {}},
                    {t7, 6, 6, 4, 0, 3, {0x41, 0x00, 0x42, 0x00}},
                    {t7, 6, 6, 3, kBufferOverflow, 2, {0x41, 0x00, 0x00}},
                    {t1, 3, 8, 5, 0, 1, {0x41, 0x00}},
                    {t1, 8, 4, 5, 0, 4, {0x41, 0x20, 0x62, 0x31, 0x00}},
                });
}

REX_TEST_CASE("rtl strings: RtlUnicodeStringToAnsiString with allocation matches the console",
              "[kernel][rtl]") {
  const std::vector<u8> t1 = Wide({0x0041, 0x0020, 0x0062, 0x0031});
  const std::vector<u8> t5 = Wide({0x0041, 0x00E9, 0x3042, 0xDC81, 0x007A});
  const std::vector<u8> t7 = Wide({0x0041, 0x0000, 0x0042});
  RunAllocatedConversion("RtlUnicodeStringToAnsiString", "RtlFreeAnsiString",
                         {
                             {t1, 8, 8, 5, 0, 4, {0x41, 0x20, 0x62, 0x31, 0x00}},
                             {Wide({0x00E9}), 2, 2, 2, 0, 1, {0xE9, 0x00}},
                             {Wide({0x3042}), 2, 2, 2, 0, 1, {0x3F, 0x00}},
                             {Wide({0xDC81}), 2, 2, 2, 0, 1, {0x3F, 0x00}},
                             {t5, 10, 10, 6, 0, 5, {0x41, 0xE9, 0x3F, 0x3F, 0x7A, 0x00}},
                             {{}, 0, 0, 1, 0, 0, {0x00}},
                             {t7, 6, 6, 4, 0, 3, {0x41, 0x00, 0x42, 0x00}},
                             {t1, 3, 8, 2, 0, 1, {0x41, 0x00}},
                             {t1, 8, 4, 5, 0, 4, {0x41, 0x20, 0x62, 0x31, 0x00}},
                         });
}

REX_TEST_CASE("rtl strings: RtlAnsiStringToUnicodeString into a caller buffer matches the console",
              "[kernel][rtl]") {
  const std::vector<u8> b1 = {0x41, 0x62, 0x31};
  const std::vector<u8> b6 = {0x41, 0x00, 0x42};
  RunConversion("RtlAnsiStringToUnicodeString",
                {
                    {b1, 3, 3, 8, 0, 6, {0x00, 0x41, 0x00, 0x62, 0x00, 0x31, 0x00, 0x00}},
                    {b1, 3, 3, 6, kBufferOverflow, 6, {}},
                    {b1, 3, 3, 2, kBufferOverflow, 6, {}},
                    {b1, 3, 3, 0, kBufferOverflow, 6, {}},
                    {{0xE9}, 1, 1, 4, 0, 2, {0x00, 0xE9, 0x00, 0x00}},
                    {{0xE9}, 1, 1, 2, kBufferOverflow, 2, {}},
                    {{0x80}, 1, 1, 4, 0, 2, {0x00, 0x80, 0x00, 0x00}},
                    {{0x8D}, 1, 1, 4, 0, 2, {0x00, 0x8D, 0x00, 0x00}},
                    {{0x95}, 1, 1, 4, 0, 2, {0x00, 0x95, 0x00, 0x00}},
                    {{0x9F}, 1, 1, 4, 0, 2, {0x00, 0x9F, 0x00, 0x00}},
                    {{0xFF}, 1, 1, 4, 0, 2, {0x00, 0xFF, 0x00, 0x00}},
                    {{0xFF}, 1, 1, 2, kBufferOverflow, 2, {}},
                    {b6, 3, 3, 8, 0, 6, {0x00, 0x41, 0x00, 0x00, 0x00, 0x42, 0x00, 0x00}},
                    {b6, 3, 3, 6, kBufferOverflow, 6, {}},
                    {{}, 0, 0, 2, 0, 0, {0x00, 0x00}},
                    {{}, 0, 0, 0, kBufferOverflow, 0, {}},
                });
}

REX_TEST_CASE("rtl strings: RtlAnsiStringToUnicodeString with allocation matches the console",
              "[kernel][rtl]") {
  RunAllocatedConversion(
      "RtlAnsiStringToUnicodeString", "RtlFreeUnicodeString",
      {
          {{0x41, 0x62, 0x31}, 3, 3, 8, 0, 6, {0x00, 0x41, 0x00, 0x62, 0x00, 0x31, 0x00, 0x00}},
          {{0xE9}, 1, 1, 4, 0, 2, {0x00, 0xE9, 0x00, 0x00}},
          {{0x80}, 1, 1, 4, 0, 2, {0x00, 0x80, 0x00, 0x00}},
          {{0x95}, 1, 1, 4, 0, 2, {0x00, 0x95, 0x00, 0x00}},
          {{0xFF}, 1, 1, 4, 0, 2, {0x00, 0xFF, 0x00, 0x00}},
          {{0x41, 0x00, 0x42}, 3, 3, 8, 0, 6, {0x00, 0x41, 0x00, 0x00, 0x00, 0x42, 0x00, 0x00}},
          {{}, 0, 0, 2, 0, 0, {0x00, 0x00}},
      });
}

REX_TEST_CASE("rtl strings: RtlMultiByteToUnicodeSize reports twice the byte count",
              "[kernel][rtl]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 size = c.Alloc(4);
    const std::pair<std::vector<u8>, u32> cases[] = {
        {{0x41, 0x62, 0x31}, 6}, {{0xE9}, 2}, {{0x80}, 2}, {{0x95}, 2}, {{0xFF}, 2},
        {{0x41, 0x00, 0x42}, 6}, {{}, 0},
    };
    for (const auto& [text, expected] : cases) {
      CAPTURE(text.size());
      c.Store32(size, kStringPoison);
      CHECK(c.Kernel("RtlMultiByteToUnicodeSize",
                     {size, CopyToGuest(c, text), static_cast<u32>(text.size())}) == 0);
      CHECK(c.Load32(size) == expected);
    }
  });
}

REX_TEST_CASE("rtl strings: RtlUnicodeToMultiByteSize reports half the byte count",
              "[kernel][rtl]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 size = c.Alloc(4);
    const std::tuple<std::vector<u8>, u32, u32> cases[] = {
        {Wide({0x0041, 0x0020, 0x0062, 0x0031}), 8, 4},
        {Wide({0x00E9}), 2, 1},
        {Wide({0x3042}), 2, 1},
        {Wide({0xDC81}), 2, 1},
        {Wide({0x0041, 0x00E9, 0x3042, 0xDC81, 0x007A}), 10, 5},
        {{}, 0, 0},
        {Wide({0x0041, 0x0000, 0x0042}), 6, 3},
        {Wide({0x0041, 0x0020, 0x0062, 0x0031}), 3, 1},
    };
    for (const auto& [text, length, expected] : cases) {
      CAPTURE(length);
      c.Store32(size, kStringPoison);
      CHECK(c.Kernel("RtlUnicodeToMultiByteSize", {size, CopyToGuest(c, text), length}) == 0);
      CHECK(c.Load32(size) == expected);
    }
  });
}

REX_TEST_CASE("rtl strings: RtlMultiByteToUnicodeN converts like the console", "[kernel][rtl]") {
  const std::vector<u8> b1 = {0x41, 0x62, 0x31};
  const std::vector<u8> b6 = {0x41, 0x00, 0x42};
  RunConversionN("RtlMultiByteToUnicodeN",
                 {
                     {b1, 3, 6, true, 6, {0x00, 0x41, 0x00, 0x62, 0x00, 0x31}},
                     {b1, 3, 4, true, 4, {0x00, 0x41, 0x00, 0x62}},
                     {b1, 3, 1, true, 0, {}},
                     {b1, 3, 0, true, 0, {}},
                     {b1, 3, 6, false, 0, {0x00, 0x41, 0x00, 0x62, 0x00, 0x31}},
                     {b1, 3, 4, false, 0, {0x00, 0x41, 0x00, 0x62}},
                     {b1, 3, 0, false, 0, {}},
                     {{0xE9}, 1, 2, true, 2, {0x00, 0xE9}},
                     {{0xE9}, 1, 1, true, 0, {}},
                     {{0x80}, 1, 4, true, 2, {0x00, 0x80}},
                     {{0x8D}, 1, 4, true, 2, {0x00, 0x8D}},
                     {{0x95}, 1, 4, true, 2, {0x00, 0x95}},
                     {{0x9F}, 1, 4, true, 2, {0x00, 0x9F}},
                     {{0xFF}, 1, 2, true, 2, {0x00, 0xFF}},
                     {b6, 3, 6, true, 6, {0x00, 0x41, 0x00, 0x00, 0x00, 0x42}},
                     {b6, 3, 4, true, 4, {0x00, 0x41, 0x00, 0x00}},
                     {{}, 0, 0, true, 0, {}},
                     {{}, 0, 1, true, 0, {}},
                 });
}

REX_TEST_CASE("rtl strings: RtlUnicodeToMultiByteN converts like the console", "[kernel][rtl]") {
  const std::vector<u8> t1 = Wide({0x0041, 0x0020, 0x0062, 0x0031});
  const std::vector<u8> t5 = Wide({0x0041, 0x00E9, 0x3042, 0xDC81, 0x007A});
  const std::vector<u8> t7 = Wide({0x0041, 0x0000, 0x0042});
  std::vector<ConvertNCase> cases = {
      {t1, 8, 4, true, 4, {0x41, 0x20, 0x62, 0x31}},
      {t1, 8, 3, true, 3, {0x41, 0x20, 0x62}},
      {t1, 8, 0, true, 0, {}},
      {t1, 8, 4, false, 0, {0x41, 0x20, 0x62, 0x31}},
      {t1, 8, 0, false, 0, {}},
      {Wide({0x00E9}), 2, 1, true, 1, {0xE9}},
      {Wide({0x00E9}), 2, 0, true, 0, {}},
      {Wide({0x3042}), 2, 1, true, 1, {0x3F}},
      {Wide({0xDC81}), 2, 1, true, 1, {0x3F}},
      {t5, 10, 5, true, 5, {0x41, 0xE9, 0x3F, 0x3F, 0x7A}},
      {t5, 10, 4, true, 4, {0x41, 0xE9, 0x3F, 0x3F}},
      {t5, 10, 0, true, 0, {}},
      {{}, 0, 0, true, 0, {}},
      {t7, 6, 3, true, 3, {0x41, 0x00, 0x42}},
      {t7, 6, 2, true, 2, {0x41, 0x00}},
      {t1, 3, 4, true, 1, {0x41}},
      {t1, 3, 1, true, 1, {0x41}},
      {t5, 9, 5, true, 4, {0x41, 0xE9, 0x3F, 0x3F}},
  };
  for (const u16 unit :
       {0x0100, 0x0141, 0x0152, 0x20AC, 0x2022, 0x3042, 0xFFFD, 0xD800, 0xDC81, 0xDFFF}) {
    cases.push_back({Wide({unit}), 2, 4, true, 1, {0x3F}});
  }
  for (const u16 unit : {0x0080, 0x0095, 0x00A0, 0x00E9, 0x00FF}) {
    cases.push_back({Wide({unit}), 2, 4, true, 1, {static_cast<u8>(unit)}});
  }
  RunConversionN("RtlUnicodeToMultiByteN", cases);
}

REX_TEST_CASE("rtl strings: RtlUpperChar maps every byte like the console", "[kernel][rtl]") {
  static constexpr u8 kConsoleUpper[256] = {
      0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E,
      0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D,
      0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C,
      0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B,
      0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A,
      0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
      0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
      0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
      0x58, 0x59, 0x5A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86,
      0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95,
      0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4,
      0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2, 0xB3,
      0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xC0, 0xC1, 0xC2,
      0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF, 0xD0, 0xD1,
      0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF, 0xC0,
      0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF,
      0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xF7, 0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE,
      0x3F,
  };
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    for (u32 byte = 0; byte < 256; ++byte) {
      CAPTURE(byte);
      CHECK(c.Kernel("RtlUpperChar", {byte}) == kConsoleUpper[byte]);
    }
  });
}
