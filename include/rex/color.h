/**
 * @file        color.h
 * @brief       The color value type shared by every ReX layer
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <algorithm>

#include <rex/types.h>

namespace rex {

namespace detail {

constexpr u32 ColorByte(f32 channel) {
  return static_cast<u32>(std::clamp(channel, 0.0f, 1.0f) * 255.0f + 0.5f);
}

}  // namespace detail

/** A color of 4 channels in the order red, green, blue, alpha. Each channel runs from 0 to 1. */
struct Color {
  /** Red channel. */
  f32 r = 0;
  /** Green channel. */
  f32 g = 0;
  /** Blue channel. */
  f32 b = 0;
  /** Alpha channel, 1 is opaque. */
  f32 a = 1;

  /**
   * Returns a color from its channels.
   *
   * @param r Red channel.
   * @param g Green channel.
   * @param b Blue channel.
   * @param a Alpha channel, opaque when omitted.
   * @return The color.
   */
  static constexpr Color RGBA(f32 r, f32 g, f32 b, f32 a = 1) { return Color{r, g, b, a}; }

  /**
   * Returns a color from a packed 0xRRGGBB value.
   *
   * @param rrggbb The packed red, green and blue bytes, red in bits 16 to 23.
   * @param a Alpha channel.
   * @return The color.
   */
  static constexpr Color Hex(u32 rrggbb, f32 a = 1) {
    return Color{static_cast<f32>((rrggbb >> 16) & 0xFFu) / 255.0f,
                 static_cast<f32>((rrggbb >> 8) & 0xFFu) / 255.0f,
                 static_cast<f32>(rrggbb & 0xFFu) / 255.0f, a};
  }

  /**
   * Returns a color from 4 bytes.
   *
   * @param r Red byte.
   * @param g Green byte.
   * @param b Blue byte.
   * @param a Alpha byte, 255 is opaque.
   * @return The color with every byte divided by 255.
   */
  static constexpr Color FromRGBA8(u8 r, u8 g, u8 b, u8 a = 255) {
    return Color{static_cast<f32>(r) / 255.0f, static_cast<f32>(g) / 255.0f,
                 static_cast<f32>(b) / 255.0f, static_cast<f32>(a) / 255.0f};
  }

  /**
   * Returns a color from a packed 0xAARRGGBB value.
   *
   * @param argb The packed value, alpha in the high byte.
   * @return The color.
   */
  static constexpr Color FromARGB8(u32 argb) {
    return FromRGBA8(static_cast<u8>(argb >> 16), static_cast<u8>(argb >> 8), static_cast<u8>(argb),
                     static_cast<u8>(argb >> 24));
  }

  /**
   * Returns this color with another alpha.
   *
   * @param alpha The new alpha channel.
   * @return The copy with the red, green and blue channels unchanged.
   */
  constexpr Color WithAlpha(f32 alpha) const { return Color{r, g, b, alpha}; }

  /**
   * Returns this color with the red, green and blue channels multiplied.
   *
   * @param factor The multiplier. Alpha is unchanged.
   * @return The scaled copy.
   */
  constexpr Color Scaled(f32 factor) const { return Color{r * factor, g * factor, b * factor, a}; }

  /**
   * Returns the linear blend of 2 colors over every channel.
   *
   * @param from The color at t equal to 0.
   * @param to The color at t equal to 1.
   * @param t The blend position.
   * @return The blended color.
   */
  static constexpr Color Lerp(Color from, Color to, f32 t) {
    const f32 inverse = 1.0f - t;
    return Color{inverse * from.r + t * to.r, inverse * from.g + t * to.g,
                 inverse * from.b + t * to.b, inverse * from.a + t * to.a};
  }

  /**
   * Returns the packed bytes with red in the low byte, the gpu::OverlayVertex order.
   *
   * @return The value r | g << 8 | b << 16 | a << 24 with each channel clamped to 0..1 and rounded.
   */
  constexpr u32 ToRGBA8() const {
    return detail::ColorByte(r) | (detail::ColorByte(g) << 8) | (detail::ColorByte(b) << 16) |
           (detail::ColorByte(a) << 24);
  }

  /**
   * Returns the packed bytes with alpha in the high byte.
   *
   * @return The value a << 24 | r << 16 | g << 8 | b with each channel clamped to 0..1 and rounded.
   */
  constexpr u32 ToARGB8() const {
    return (detail::ColorByte(a) << 24) | (detail::ColorByte(r) << 16) |
           (detail::ColorByte(g) << 8) | detail::ColorByte(b);
  }

  /**
   * Reports whether all 4 channels are equal.
   *
   * @param other The color to compare with.
   * @return True when every channel matches exactly.
   */
  constexpr bool operator==(const Color& other) const = default;
};

namespace colors {

/** Opaque white. */
inline constexpr Color kWhite = Color::RGBA(1, 1, 1);
/** Opaque black. */
inline constexpr Color kBlack = Color::RGBA(0, 0, 0);
/** Black with alpha 0. */
inline constexpr Color kTransparent = Color::RGBA(0, 0, 0, 0);

}  // namespace colors

}  // namespace rex
