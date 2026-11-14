/**
 * @file        tests/unit/kernel/strings_test.cpp
 * @brief       Rtl string comparison and XAM string formatting
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <format>
#include <string>
#include <string_view>

#include <rex/platform.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/heap.h>

#include "kernel_fixture.h"
#include "kernel/title_state.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <rex/ppc/context.h>
#include <array>

using rex::test::Caller;
using rex::test::KernelFixture;

namespace {
constexpr u32 kStrlen = 0xFFFF;
constexpr std::time_t kUnixTime = 1709647620;
constexpr u64 kFiletime = static_cast<u64>(kUnixTime) * 10000000 + 116444736000000000;
constexpr u32 kTextChars = 32;

u32 PutString(Caller& c, std::string_view text) {
  const u32 address = c.Alloc(static_cast<u32>(text.size()) + 1);
  std::memcpy(StateOf(c.GetRuntime()).GetAddressSpace()->TranslateVirtual<char*>(address),
              text.data(), text.size());
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

REX_TEST_CASE("strings: RtlCompareStringN resolves the strlen length before it compares",
              "[kernel][strings]") {
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

REX_TEST_CASE("strings: RtlUpperChar and RtlLowerChar map Latin 1 letters", "[kernel][strings]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    CHECK(c.Kernel("RtlUpperChar", {0xE9}) == 0xC9);
    CHECK(c.Kernel("RtlLowerChar", {0xC9}) == 0xE9);
    CHECK(c.Kernel("RtlLowerChar", {0xD7}) == 0xD7);
    CHECK(c.Kernel("RtlUpperChar", {'a'}) == 'A');
    CHECK(c.Kernel("RtlLowerChar", {'A'}) == 'a');
  });
}

REX_TEST_CASE("strings: XAM formats a filetime as local date and time", "[kernel][strings]") {
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

namespace strtok_tests {

extern "C" void rexshim_strtok(PPCContext& ctx, rex::u8* base);

using rex::u32;
using rex::test::Caller;
using rex::test::KernelFixture;

namespace {

std::string Tokenize(Caller& c, u32 text, u32 delim) {
  c.GetContext().r3.u64 = text;
  c.GetContext().r4.u64 = delim;
  rexshim_strtok(c.GetContext(), c.GetBase());
  const u32 token = c.GetContext().r3.u32;
  return token ? std::string(reinterpret_cast<const char*>(c.GetBase() + token)) : std::string();
}

bool WaitForStep(const std::atomic<int>& step, int value) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (step.load() < value) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

}  // namespace

REX_TEST_CASE("Two guest threads tokenize different strings without sharing a position",
              "[kernel][strtok]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    struct Result {
      std::string first;
      std::string second;
    };
    auto a = std::make_shared<Result>();
    auto b = std::make_shared<Result>();
    auto step = std::make_shared<std::atomic<int>>(0);
    const u32 delim = c.AllocString(",");
    const u32 text_a = c.AllocString("a1,a2");
    const u32 text_b = c.AllocString("b1,b2");

    const u32 thread_a = c.Spawn([=](Caller& t) {
      a->first = Tokenize(t, text_a, delim);
      step->store(1);
      if (WaitForStep(*step, 2)) {
        a->second = Tokenize(t, 0, delim);
      }
      step->store(3);
    });
    const u32 thread_b = c.Spawn([=](Caller& t) {
      if (WaitForStep(*step, 1)) {
        b->first = Tokenize(t, text_b, delim);
      }
      step->store(2);
      if (WaitForStep(*step, 3)) {
        b->second = Tokenize(t, 0, delim);
      }
    });
    rex::test::Join(c, thread_a);
    rex::test::Join(c, thread_b);

    CHECK(a->first == "a1");
    CHECK(b->first == "b1");
    CHECK(a->second == "a2");
    CHECK(b->second == "b2");
  });
}

}  // namespace strtok_tests

namespace rtl_time_tests {

using rex::test::Caller;
using rex::test::KernelFixture;

namespace {

struct Calendar {
  u64 filetime;
  u32 year;
  u32 month;
  u32 day;
  u32 hour;
  u32 minute;
  u32 second;
  u32 milliseconds;
  u32 weekday;
};

constexpr std::array<Calendar, 6> kCalendar = {{
    {0, 1601, 1, 1, 0, 0, 0, 0, 1},
    {116444736000000000, 1970, 1, 1, 0, 0, 0, 0, 4},
    {125911584000000000, 2000, 1, 1, 0, 0, 0, 0, 6},
    {125962560000000000, 2000, 2, 29, 0, 0, 0, 0, 2},
    {132538032123450000, 2020, 12, 30, 12, 0, 12, 345, 3},
    {132539328000000000, 2021, 1, 1, 0, 0, 0, 0, 5},
}};

u32 Field(Caller& c, u32 fields, u32 index) {
  const u32 pair = c.Load32(fields + (index / 2) * 4);
  return (index % 2 == 0 ? pair >> 16 : pair) & 0xFFFF;
}

u32 MakeFields(Caller& c, const Calendar& item) {
  const u32 fields = c.Alloc(16);
  c.Store32(fields, (item.year << 16) | item.month);
  c.Store32(fields + 4, (item.day << 16) | item.hour);
  c.Store32(fields + 8, (item.minute << 16) | item.second);
  c.Store32(fields + 12, (item.milliseconds << 16) | item.weekday);
  return fields;
}

}  // namespace

REX_TEST_CASE("rtl time: RtlTimeToTimeFields splits known FILETIMEs", "[kernel][rtl]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    for (const Calendar& item : kCalendar) {
      INFO(item.filetime);
      const u32 filetime_out = c.Alloc(8);
      c.Store64(filetime_out, item.filetime);
      const u32 fields = c.Alloc(16);
      c.Kernel("RtlTimeToTimeFields", {filetime_out, fields});
      CHECK(Field(c, fields, 0) == item.year);
      CHECK(Field(c, fields, 1) == item.month);
      CHECK(Field(c, fields, 2) == item.day);
      CHECK(Field(c, fields, 3) == item.hour);
      CHECK(Field(c, fields, 4) == item.minute);
      CHECK(Field(c, fields, 5) == item.second);
      CHECK(Field(c, fields, 6) == item.milliseconds);
      CHECK(Field(c, fields, 7) == item.weekday);
    }
  });
}

REX_TEST_CASE("rtl time: RtlTimeFieldsToTime joins known dates", "[kernel][rtl]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    for (const Calendar& item : kCalendar) {
      INFO(item.filetime);
      const u32 filetime_out = c.Alloc(8);
      CHECK(c.Kernel("RtlTimeFieldsToTime", {MakeFields(c, item), filetime_out}) == 1);
      CHECK(((static_cast<u64>(c.Load32(filetime_out)) << 32) | c.Load32(filetime_out + 4)) ==
            item.filetime);
    }
  });
}

REX_TEST_CASE("rtl time: RtlTimeFieldsToTime fails on fields out of range", "[kernel][rtl]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 filetime_out = c.Alloc(8);
    for (const Calendar& item : {
             Calendar{0, 2000, 2, 30, 0, 0, 0, 0, 0},
             Calendar{0, 2001, 2, 29, 0, 0, 0, 0, 0},
             Calendar{0, 2000, 13, 1, 0, 0, 0, 0, 0},
             Calendar{0, 2000, 1, 0, 0, 0, 0, 0, 0},
             Calendar{0, 1600, 1, 1, 0, 0, 0, 0, 0},
             Calendar{0, 2000, 1, 1, 24, 0, 0, 0, 0},
             Calendar{0, 2000, 1, 1, 0, 60, 0, 0, 0},
             Calendar{0, 2000, 1, 1, 0, 0, 60, 0, 0},
             Calendar{0, 2000, 1, 1, 0, 0, 0, 1000, 0},
         }) {
      INFO(item.year << "-" << item.month << "-" << item.day);
      CHECK(c.Kernel("RtlTimeFieldsToTime", {MakeFields(c, item), filetime_out}) == 0);
    }
  });
}

}  // namespace rtl_time_tests
