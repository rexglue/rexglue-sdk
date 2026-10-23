/**
 * @file        tests/unit/kernel/shim_math_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <clocale>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

#include <rex/platform.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/ppc/func.h>

using namespace rex::ppc;

extern "C" REX_FUNC(rexshim_atan2);
extern "C" REX_FUNC(rexshim_atof);

namespace {

alignas(64) u8 g_guest[0x10000];

constexpr u32 kText = 0x1000;

bool IsNear(f64 value, f64 expected) {
  return std::abs(value - expected) < 1e-12;
}

f64 CallAtan2(f64 y, f64 x) {
  PPCContext ctx{};
  ctx.f1.f64 = y;
  ctx.f2.f64 = x;
  rexshim_atan2(ctx, g_guest);
  return ctx.f1.f64;
}

f64 CallAtof(std::string_view text) {
  std::memset(g_guest, 0xEE, sizeof(g_guest));
  std::memcpy(g_guest + kText, text.data(), text.size());
  g_guest[kText + text.size()] = 0;
  PPCContext ctx{};
  ctx.r3.u64 = kText;
  rexshim_atof(ctx, g_guest);
  return ctx.f1.f64;
}

bool UseCommaLocale() {
  constexpr const char* kCandidates[] = {
      "de_DE.UTF-8", "de_DE.utf8",          "de_DE", "fr_FR.UTF-8", "fr_FR.utf8",
      "fr_FR",       "German_Germany.1252", "de-DE", "fr-FR",       "ru_RU.UTF-8",
  };
  for (const char* name : kCandidates) {
    if (std::setlocale(LC_ALL, name) != nullptr) {
      const std::lconv* conv = std::localeconv();
      if (conv && std::strcmp(conv->decimal_point, ",") == 0) {
        return true;
      }
    }
  }
  std::setlocale(LC_ALL, "C");
  return false;
}

}  // namespace

REX_TEST_CASE("CRT atan2 matches the standard result for each quadrant",
              "[kernel][shim][crt][math]") {
  const f64 pi = 3.14159265358979323846;
  CHECK(IsNear(CallAtan2(1.0, 1.0), pi / 4));
  CHECK(IsNear(CallAtan2(1.0, -1.0), 3 * pi / 4));
  CHECK(IsNear(CallAtan2(-1.0, -1.0), -3 * pi / 4));
  CHECK(IsNear(CallAtan2(-1.0, 1.0), -pi / 4));
  CHECK(IsNear(CallAtan2(0.0, -1.0), pi));
  CHECK(IsNear(CallAtan2(-1.0, 0.0), -pi / 2));
  CHECK(CallAtan2(0.0, 0.0) == 0.0);
}

REX_TEST_CASE("CRT atof parses a leading number", "[kernel][shim][crt][math]") {
  CHECK(CallAtof("3.5") == 3.5);
  CHECK(CallAtof("  \t\n-2.25abc") == -2.25);
  CHECK(CallAtof("+7") == 7.0);
  CHECK(CallAtof("1e3") == 1000.0);
  CHECK(CallAtof("2.5E-1xyz") == 0.25);
  CHECK(CallAtof(".5") == 0.5);
  CHECK(CallAtof("12 34") == 12.0);
  CHECK(CallAtof("1,5") == 1.0);
}

REX_TEST_CASE("CRT atof returns zero when no digits are present", "[kernel][shim][crt][math]") {
  CHECK(CallAtof("") == 0.0);
  CHECK(CallAtof("abc") == 0.0);
  CHECK(CallAtof("   ") == 0.0);
  CHECK(CallAtof("-") == 0.0);
  CHECK(CallAtof(".") == 0.0);
}

REX_TEST_CASE("CRT atof saturates out of range numbers", "[kernel][shim][crt][math]") {
  CHECK(CallAtof("1e999") == std::numeric_limits<f64>::infinity());
  CHECK(CallAtof("-1e999") == -std::numeric_limits<f64>::infinity());
  CHECK(CallAtof("1e-999") == 0.0);
}

REX_TEST_CASE("CRT atof ignores the host locale", "[kernel][shim][crt][math]") {
  const std::string previous = std::setlocale(LC_ALL, nullptr);
  const bool changed = UseCommaLocale();
#if REX_PLATFORM_WIN32
  REQUIRE(changed);
#else
  if (!changed) {
    WARN("No locale with a decimal comma is installed on this host");
  }
#endif
  CHECK(CallAtof("1.5") == 1.5);
  CHECK(CallAtof("-0.25") == -0.25);
  CHECK(CallAtof("1,5") == 1.0);
  CHECK(CallAtof("2.5e2") == 250.0);
  std::setlocale(LC_ALL, previous.c_str());
}
