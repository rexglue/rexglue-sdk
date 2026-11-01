/**
 * @file        tests/unit/base/color_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/ui/style.h>

using rex::ui::Color;
namespace colors = rex::ui::colors;

static_assert(rex::ui::Color::FromRGBA(1, 1, 1) == rex::ui::colors::kWhite);
static_assert(rex::ui::Color::FromRGBA(0, 0, 0, 0) == rex::ui::colors::kTransparent);
static_assert(rex::ui::colors::kBlack.ToRGBA8() == 0xFF000000u);

REX_TEST_CASE("Color defaults to opaque black", "[base][color]") {
  const rex::ui::Color color{};
  CHECK(color.r == 0.0f);
  CHECK(color.g == 0.0f);
  CHECK(color.b == 0.0f);
  CHECK(color.a == 1.0f);
}

REX_TEST_CASE("Color RGBA fills the channels and defaults to opaque", "[base][color]") {
  const rex::ui::Color opaque = rex::ui::Color::FromRGBA(0.25f, 0.5f, 0.75f);
  CHECK(opaque.r == 0.25f);
  CHECK(opaque.g == 0.5f);
  CHECK(opaque.b == 0.75f);
  CHECK(opaque.a == 1.0f);
  const rex::ui::Color rgba = rex::ui::Color::FromRGBA(0.1f, 0.2f, 0.3f, 0.4f);
  CHECK(rgba.r == 0.1f);
  CHECK(rgba.g == 0.2f);
  CHECK(rgba.b == 0.3f);
  CHECK(rgba.a == 0.4f);
}

REX_TEST_CASE("Color Hex reads 0xRRGGBB", "[base][color]") {
  const rex::ui::Color hex = rex::ui::Color::FromRGB8(0xFF8000);
  CHECK(hex.r == 1.0f);
  CHECK(hex.g == 128.0f / 255.0f);
  CHECK(hex.b == 0.0f);
  CHECK(hex.a == 1.0f);
  CHECK(rex::ui::Color::FromRGB8(0x000000, 0.5f).a == 0.5f);
  CHECK(rex::ui::Color::FromRGB8(0x336699).ToARGB8() == 0xFF336699u);
}

REX_TEST_CASE("Color FromRGBA8 scales bytes to channels", "[base][color]") {
  const rex::ui::Color color = rex::ui::Color::FromRGBA8(255, 128, 0);
  CHECK(color.r == 1.0f);
  CHECK(color.g == 128.0f / 255.0f);
  CHECK(color.b == 0.0f);
  CHECK(color.a == 1.0f);
  CHECK(rex::ui::Color::FromRGBA8(0, 0, 0, 51).a == 51.0f / 255.0f);
  CHECK(rex::ui::Color::FromRGBA8(12, 34, 56, 78).ToRGBA8() == 0x4E38220Cu);
  CHECK(rex::ui::Color::FromRGBA8(12, 34, 56, 78).ToARGB8() == 0x4E0C2238u);
}

REX_TEST_CASE("Color FromARGB8 reads alpha from the high byte", "[base][color]") {
  const rex::ui::Color color = rex::ui::Color::FromARGB8(0x80FF0000);
  CHECK(color.r == 1.0f);
  CHECK(color.g == 0.0f);
  CHECK(color.b == 0.0f);
  CHECK(color.a == 128.0f / 255.0f);
  CHECK(color.ToARGB8() == 0x80FF0000u);
}

REX_TEST_CASE("Color packs red low in ToRGBA8 and alpha high in both", "[base][color]") {
  CHECK(rex::ui::Color::FromRGBA(1, 0, 0).ToRGBA8() == 0xFF0000FFu);
  CHECK(rex::ui::Color::FromRGBA(1, 0, 0).ToARGB8() == 0xFFFF0000u);
  CHECK(rex::ui::Color::FromRGBA(0, 0, 1).ToRGBA8() == 0xFFFF0000u);
  CHECK(rex::ui::Color::FromRGBA(0, 0, 1).ToARGB8() == 0xFF0000FFu);
}

REX_TEST_CASE("Named constants round trip through both packed forms", "[base][color]") {
  CHECK(rex::ui::colors::kWhite.ToRGBA8() == 0xFFFFFFFFu);
  CHECK(rex::ui::colors::kWhite.ToARGB8() == 0xFFFFFFFFu);
  CHECK(rex::ui::colors::kBlack.ToRGBA8() == 0xFF000000u);
  CHECK(rex::ui::colors::kBlack.ToARGB8() == 0xFF000000u);
  CHECK(rex::ui::colors::kTransparent.ToRGBA8() == 0u);
  CHECK(rex::ui::colors::kTransparent.ToARGB8() == 0u);
  for (const rex::ui::Color color :
       {rex::ui::colors::kWhite, rex::ui::colors::kBlack, rex::ui::colors::kTransparent}) {
    CHECK(rex::ui::Color::FromARGB8(color.ToARGB8()) == color);
    const rex::u32 rgba = color.ToRGBA8();
    CHECK(rex::ui::Color::FromRGBA8(static_cast<rex::u8>(rgba), static_cast<rex::u8>(rgba >> 8),
                                    static_cast<rex::u8>(rgba >> 16),
                                    static_cast<rex::u8>(rgba >> 24)) == color);
  }
}

REX_TEST_CASE("Color Lerp blends every channel", "[base][color]") {
  CHECK(rex::ui::Color::Lerp(rex::ui::colors::kBlack, rex::ui::colors::kWhite, 0.0f) ==
        rex::ui::colors::kBlack);
  CHECK(rex::ui::Color::Lerp(rex::ui::colors::kBlack, rex::ui::colors::kWhite, 1.0f) ==
        rex::ui::colors::kWhite);
  CHECK(rex::ui::Color::Lerp(rex::ui::colors::kBlack, rex::ui::colors::kWhite, 0.5f) ==
        rex::ui::Color::FromRGBA(0.5f, 0.5f, 0.5f));
  CHECK(rex::ui::Color::Lerp(rex::ui::colors::kBlack, rex::ui::colors::kWhite, 0.5f).ToRGBA8() ==
        0xFF808080u);
  CHECK(rex::ui::Color::Lerp(rex::ui::colors::kTransparent, rex::ui::colors::kWhite, 0.5f).a ==
        0.5f);
}

REX_TEST_CASE("Color WithAlpha replaces only alpha", "[base][color]") {
  const rex::ui::Color faded = rex::ui::colors::kWhite.WithAlpha(0.3f);
  CHECK(faded == rex::ui::Color::FromRGBA(1, 1, 1, 0.3f));
  CHECK(rex::ui::colors::kWhite.a == 1.0f);
}

REX_TEST_CASE("Color Scaled multiplies rgb and keeps alpha", "[base][color]") {
  const rex::ui::Color scaled = rex::ui::Color::FromRGBA(0.5f, 0.25f, 1.0f, 0.7f).Scaled(0.5f);
  CHECK(scaled == rex::ui::Color::FromRGBA(0.25f, 0.125f, 0.5f, 0.7f));
}

REX_TEST_CASE("Color packing clamps and rounds to nearest", "[base][color]") {
  const rex::ui::Color wild = rex::ui::Color::FromRGBA(2.0f, -1.0f, 0.5f, 3.0f);
  CHECK(wild.ToRGBA8() == 0xFF8000FFu);
  CHECK(wild.ToARGB8() == 0xFFFF0080u);
  CHECK((rex::ui::Color::FromRGBA(2, 0, 0).ToRGBA8() & 0xFFu) == 255u);
}
