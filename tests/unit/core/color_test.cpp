/**
 * @file        tests/unit/core/color_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/color.h>

using rex::Color;
namespace colors = rex::colors;

static_assert(Color::RGBA(1, 1, 1) == colors::kWhite);
static_assert(Color::RGBA(0, 0, 0, 0) == colors::kTransparent);
static_assert(colors::kBlack.ToRGBA8() == 0xFF000000u);

TEST_CASE("Color defaults to opaque black", "[color]") {
  const Color color{};
  CHECK(color.r == 0.0f);
  CHECK(color.g == 0.0f);
  CHECK(color.b == 0.0f);
  CHECK(color.a == 1.0f);
}

TEST_CASE("Color RGBA fills the channels and defaults to opaque", "[color]") {
  const Color opaque = Color::RGBA(0.25f, 0.5f, 0.75f);
  CHECK(opaque.r == 0.25f);
  CHECK(opaque.g == 0.5f);
  CHECK(opaque.b == 0.75f);
  CHECK(opaque.a == 1.0f);
  const Color rgba = Color::RGBA(0.1f, 0.2f, 0.3f, 0.4f);
  CHECK(rgba.r == 0.1f);
  CHECK(rgba.g == 0.2f);
  CHECK(rgba.b == 0.3f);
  CHECK(rgba.a == 0.4f);
}

TEST_CASE("Color Hex reads 0xRRGGBB", "[color]") {
  const Color hex = Color::Hex(0xFF8000);
  CHECK(hex.r == 1.0f);
  CHECK(hex.g == 128.0f / 255.0f);
  CHECK(hex.b == 0.0f);
  CHECK(hex.a == 1.0f);
  CHECK(Color::Hex(0x000000, 0.5f).a == 0.5f);
  CHECK(Color::Hex(0x336699).ToARGB8() == 0xFF336699u);
}

TEST_CASE("Color FromRGBA8 scales bytes to channels", "[color]") {
  const Color color = Color::FromRGBA8(255, 128, 0);
  CHECK(color.r == 1.0f);
  CHECK(color.g == 128.0f / 255.0f);
  CHECK(color.b == 0.0f);
  CHECK(color.a == 1.0f);
  CHECK(Color::FromRGBA8(0, 0, 0, 51).a == 51.0f / 255.0f);
  CHECK(Color::FromRGBA8(12, 34, 56, 78).ToRGBA8() == 0x4E38220Cu);
  CHECK(Color::FromRGBA8(12, 34, 56, 78).ToARGB8() == 0x4E0C2238u);
}

TEST_CASE("Color FromARGB8 reads alpha from the high byte", "[color]") {
  const Color color = Color::FromARGB8(0x80FF0000);
  CHECK(color.r == 1.0f);
  CHECK(color.g == 0.0f);
  CHECK(color.b == 0.0f);
  CHECK(color.a == 128.0f / 255.0f);
  CHECK(color.ToARGB8() == 0x80FF0000u);
}

TEST_CASE("Color packs red low in ToRGBA8 and alpha high in both", "[color]") {
  CHECK(Color::RGBA(1, 0, 0).ToRGBA8() == 0xFF0000FFu);
  CHECK(Color::RGBA(1, 0, 0).ToARGB8() == 0xFFFF0000u);
  CHECK(Color::RGBA(0, 0, 1).ToRGBA8() == 0xFFFF0000u);
  CHECK(Color::RGBA(0, 0, 1).ToARGB8() == 0xFF0000FFu);
}

TEST_CASE("Named constants round trip through both packed forms", "[color]") {
  CHECK(colors::kWhite.ToRGBA8() == 0xFFFFFFFFu);
  CHECK(colors::kWhite.ToARGB8() == 0xFFFFFFFFu);
  CHECK(colors::kBlack.ToRGBA8() == 0xFF000000u);
  CHECK(colors::kBlack.ToARGB8() == 0xFF000000u);
  CHECK(colors::kTransparent.ToRGBA8() == 0u);
  CHECK(colors::kTransparent.ToARGB8() == 0u);
  for (const Color color : {colors::kWhite, colors::kBlack, colors::kTransparent}) {
    CHECK(Color::FromARGB8(color.ToARGB8()) == color);
    const rex::u32 rgba = color.ToRGBA8();
    CHECK(Color::FromRGBA8(static_cast<rex::u8>(rgba), static_cast<rex::u8>(rgba >> 8),
                           static_cast<rex::u8>(rgba >> 16),
                           static_cast<rex::u8>(rgba >> 24)) == color);
  }
}

TEST_CASE("Color Lerp blends every channel", "[color]") {
  CHECK(Color::Lerp(colors::kBlack, colors::kWhite, 0.0f) == colors::kBlack);
  CHECK(Color::Lerp(colors::kBlack, colors::kWhite, 1.0f) == colors::kWhite);
  CHECK(Color::Lerp(colors::kBlack, colors::kWhite, 0.5f) == Color::RGBA(0.5f, 0.5f, 0.5f));
  CHECK(Color::Lerp(colors::kBlack, colors::kWhite, 0.5f).ToRGBA8() == 0xFF808080u);
  CHECK(Color::Lerp(colors::kTransparent, colors::kWhite, 0.5f).a == 0.5f);
}

TEST_CASE("Color WithAlpha replaces only alpha", "[color]") {
  const Color faded = colors::kWhite.WithAlpha(0.3f);
  CHECK(faded == Color::RGBA(1, 1, 1, 0.3f));
  CHECK(colors::kWhite.a == 1.0f);
}

TEST_CASE("Color Scaled multiplies rgb and keeps alpha", "[color]") {
  const Color scaled = Color::RGBA(0.5f, 0.25f, 1.0f, 0.7f).Scaled(0.5f);
  CHECK(scaled == Color::RGBA(0.25f, 0.125f, 0.5f, 0.7f));
}

TEST_CASE("Color packing clamps and rounds to nearest", "[color]") {
  const Color wild = Color::RGBA(2.0f, -1.0f, 0.5f, 3.0f);
  CHECK(wild.ToRGBA8() == 0xFF8000FFu);
  CHECK(wild.ToARGB8() == 0xFFFF0080u);
  CHECK((Color::RGBA(2, 0, 0).ToRGBA8() & 0xFFu) == 255u);
}
