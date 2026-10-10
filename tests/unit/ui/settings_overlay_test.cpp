/**
 * @file        tests/unit/ui/settings_overlay_test.cpp
 * @brief       Integer cvars keep their full width through a settings overlay edit
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <string>

#include <rex/cvar.h>
#include <rex/types.h>
#include "ui/sdl3/overlay/settings_overlay.h"

namespace {

rex::cvar::Desc IntegerDesc(rex::cvar::Type type) {
  rex::cvar::Desc desc;
  desc.name = "test.value";
  desc.type = type;
  return desc;
}

}  // namespace

REX_TEST_CASE("an Int64 cvar beyond 32 bits survives an edit round trip", "[ui][settings]") {
  const std::string text = std::to_string(i64{1} << 40);
  CHECK(rex::ui::ClampIntegerCvarText(IntegerDesc(rex::cvar::Type::Int64), text) == text);
}

REX_TEST_CASE("a UInt64 cvar beyond 32 bits survives an edit round trip", "[ui][settings]") {
  const std::string text = std::to_string(u64{1} << 40);
  CHECK(rex::ui::ClampIntegerCvarText(IntegerDesc(rex::cvar::Type::UInt64), text) == text);
}

REX_TEST_CASE("an integer cvar clamps to its constraints", "[ui][settings]") {
  rex::cvar::Desc desc = IntegerDesc(rex::cvar::Type::Int64);
  desc.constraints.min = 2.0;
  desc.constraints.max = 8.0;
  CHECK(rex::ui::ClampIntegerCvarText(desc, "1") == "2");
  CHECK(rex::ui::ClampIntegerCvarText(desc, "9") == "8");
  CHECK(rex::ui::ClampIntegerCvarText(desc, "5") == "5");
}
