/**
 * @file        tests/unit/kernel/char_strings_test.cpp
 * @brief       Rtl compares and Xam formats strings the way the title asks
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <format>
#include <string>
#include <string_view>

#include <rex/platform.h>
#include <rex/runtime.h>
#include <rex/runtime/xmemory.h>

#include "kernel_fixture.h"

using rex::testing::Caller;
using rex::testing::KernelFixture;

namespace {
constexpr u32 kStrlen = 0xFFFF;
constexpr std::time_t kUnixTime = 1709647620;
constexpr u64 kFiletime = static_cast<u64>(kUnixTime) * 10000000 + 116444736000000000;
constexpr u32 kTextChars = 32;

u32 PutString(Caller& c, std::string_view text) {
  const u32 address = c.Alloc(static_cast<u32>(text.size()) + 1);
  std::memcpy(c.runtime().memory()->TranslateVirtual<char*>(address), text.data(), text.size());
  return address;
}

std::string ReadText(Caller& c, u32 address) {
  std::string text;
  for (u32 i = 0; i < kTextChars; ++i) {
    const u32 code = (u32{c.Load8(address + i * 2)} << 8) | c.Load8(address + i * 2 + 1);
    if (!code) {
      break;
    }
    text.push_back(static_cast<char>(code));
  }
  return text;
}

std::tm LocalTime(std::time_t time) {
  std::tm local = {};
#if REX_PLATFORM_WIN32
  localtime_s(&local, &time);
#else
  localtime_r(&time, &local);
#endif
  return local;
}
}  // namespace

TEST_CASE("strings: RtlCompareStringN resolves the strlen length before it compares",
          "[char][strings]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 shorter = PutString(c, "ab");
    const u32 longer = PutString(c, "abX");
    for (u32 case_insensitive : {0u, 1u}) {
      CHECK(c.Kernel("RtlCompareStringN", {shorter, kStrlen, longer, 3, case_insensitive}) == 0);
      CHECK(c.Kernel("RtlCompareStringN", {longer, 3, shorter, kStrlen, case_insensitive}) == 0);
    }
  });
}

TEST_CASE("strings: RtlUpperChar and RtlLowerChar keep a character above 0x7F", "[char][strings]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    CHECK(c.Kernel("RtlUpperChar", {0xE9}) == 0xE9);
    CHECK(c.Kernel("RtlLowerChar", {0xC9}) == 0xC9);
    CHECK(c.Kernel("RtlUpperChar", {'a'}) == 'A');
    CHECK(c.Kernel("RtlLowerChar", {'A'}) == 'a');
  });
}

TEST_CASE("strings: Xam formats a filetime as local date and time", "[char][strings]") {
#if !REX_PLATFORM_WIN32
  setenv("TZ", "UTC", 1);
  tzset();
#endif
  const std::tm local = LocalTime(kUnixTime);
  const std::string date =
      std::format("{:02d}/{:02d}/{}", local.tm_mon + 1, local.tm_mday, local.tm_year + 1900);
  const std::string time = std::format("{:02d}:{:02d}", local.tm_hour, local.tm_min);
#if !REX_PLATFORM_WIN32
  CHECK(date == "03/05/2024");
  CHECK(time == "14:07");
#endif
  KernelFixture fixture;
  fixture.RunOnGuestThread([&](Caller& c) {
    const u32 buffer = c.Alloc(kTextChars * 2);
    c.XAM("XamFormatDateString", {0, kFiletime, buffer, kTextChars});
    CHECK(ReadText(c, buffer) == date);
    c.XAM("XamFormatTimeString", {0, kFiletime, buffer, kTextChars});
    CHECK(ReadText(c, buffer) == time);
  });
}
