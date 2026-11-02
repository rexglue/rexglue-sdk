/**
 * @file        tests/unit/kernel/rtl_time_test.cpp
 * @brief       Rtl calendar conversions between FILETIME and time fields
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <array>

#include "kernel_fixture.h"

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
