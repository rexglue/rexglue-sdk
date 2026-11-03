/**
 * @file        ui/style.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <imgui.h>
#include <algorithm>

#include <rex/math.h>
#include <rex/logging/log.h>
#include <rex/types.h>
#include <rex/platform.h>

namespace rex::ui {

namespace detail {

constexpr u32 ColorByte(f32 channel) {
  const f32 scaled = std::clamp(channel, 0.0f, 1.0f) * 255.0f;
  const u32 whole = static_cast<u32>(scaled);
  return whole + ((scaled - static_cast<f32>(whole)) >= 0.5f ? 1u : 0u);
}

}  // namespace detail

/** A color of four channels in the order red, green, blue, alpha. Each channel runs from 0 to 1. */
struct Color {
  f32 r = 0;
  f32 g = 0;
  f32 b = 0;
  f32 a = 1; /**< 1 is opaque. */

  /**
   * Returns a color from its channels.
   *
   * @param a Alpha channel, opaque when omitted.
   */
  static constexpr Color FromRGBA(f32 r, f32 g, f32 b, f32 a = 1) { return Color{r, g, b, a}; }

  /**
   * Returns a color from a packed 0xRRGGBB value.
   *
   * @param rrggbb The packed red, green and blue bytes, red in bits 16 to 23.
   */
  static constexpr Color FromRGB8(u32 rrggbb, f32 a = 1) {
    return Color{static_cast<f32>((rrggbb >> 16) & 0xFFu) / 255.0f,
                 static_cast<f32>((rrggbb >> 8) & 0xFFu) / 255.0f,
                 static_cast<f32>(rrggbb & 0xFFu) / 255.0f, a};
  }

  /**
   * Returns a color from four bytes. Every byte is divided by 255.
   *
   * @param r Red byte.
   * @param g Green byte.
   * @param b Blue byte.
   * @param a Alpha byte, 255 is opaque.
   */
  static constexpr Color FromRGBA8(u8 r, u8 g, u8 b, u8 a = 255) {
    return Color{static_cast<f32>(r) / 255.0f, static_cast<f32>(g) / 255.0f,
                 static_cast<f32>(b) / 255.0f, static_cast<f32>(a) / 255.0f};
  }

  /**
   * Returns a color from a packed 0xAARRGGBB value.
   *
   * @param argb The packed value, alpha in the high byte.
   */
  static constexpr Color FromARGB8(u32 argb) {
    return FromRGBA8(static_cast<u8>(argb >> 16), static_cast<u8>(argb >> 8), static_cast<u8>(argb),
                     static_cast<u8>(argb >> 24));
  }

  /** Returns this color with another alpha. The red, green and blue channels stay unchanged. */
  constexpr Color WithAlpha(f32 alpha) const { return Color{r, g, b, alpha}; }

  /**
   * Returns this color with red, green and blue multiplied.
   *
   * @param factor The multiplier. Alpha is unchanged.
   */
  constexpr Color Scaled(f32 factor) const { return Color{r * factor, g * factor, b * factor, a}; }

  /**
   * Returns the linear blend of two colors over every channel.
   *
   * @param from The color at t equal to 0.
   * @param to The color at t equal to 1.
   */
  static constexpr Color Lerp(Color from, Color to, f32 t) {
    const f32 inverse = 1.0f - t;
    return Color{inverse * from.r + t * to.r, inverse * from.g + t * to.g,
                 inverse * from.b + t * to.b, inverse * from.a + t * to.a};
  }

  /**
   * Returns the packed bytes with red in the low byte, the gpu::OverlayVertex order. The value is r
   * | g << 8 | b << 16 | a << 24 with each channel clamped to 0..1 and rounded.
   */
  constexpr u32 ToRGBA8() const {
    return detail::ColorByte(r) | (detail::ColorByte(g) << 8) | (detail::ColorByte(b) << 16) |
           (detail::ColorByte(a) << 24);
  }

  /**
   * Returns the packed bytes with alpha in the high byte. The value is a << 24 | r << 16 | g << 8 |
   * b with each channel clamped to 0..1 and rounded.
   */
  constexpr u32 ToARGB8() const {
    return (detail::ColorByte(a) << 24) | (detail::ColorByte(r) << 16) |
           (detail::ColorByte(g) << 8) | detail::ColorByte(b);
  }

  constexpr bool operator==(const Color& other) const = default;
};

namespace colors {

inline constexpr Color kWhite = Color::FromRGBA(1, 1, 1);
inline constexpr Color kBlack = Color::FromRGBA(0, 0, 0);
inline constexpr Color kTransparent = Color::FromRGBA(0, 0, 0, 0);

}  // namespace colors

}  // namespace rex::ui

namespace rex::ui {

namespace detail {

inline constexpr Color kSuccess = Color::FromRGBA(0.40f, 1.00f, 0.40f);
inline constexpr Color kWarning = Color::FromRGBA(1.00f, 0.80f, 0.20f);
inline constexpr Color kError = Color::FromRGBA(1.00f, 0.40f, 0.40f);
inline constexpr Color kInfo = Color::FromRGBA(0.60f, 0.85f, 1.00f);
inline constexpr Color kMuted = Color::FromRGBA(1.00f, 1.00f, 1.00f, 0.50f);

}  // namespace detail

/** Widget colors with no semantic role, mapped one to one onto ImGui slots. */
struct ControlPalette {
  /** Input frame background. */
  Color frame = Color::FromRGBA(0.80f, 0.80f, 0.80f, 0.30f);
  Color frame_hovered = Color::FromRGBA(0.90f, 0.80f, 0.80f, 0.40f);
  Color frame_active = Color::FromRGBA(0.90f, 0.65f, 0.65f, 0.45f);
  /** Window title bar and the base of the header colors. */
  Color title = Color::FromRGBA(0.00f, 0.40f, 0.00f, 1.00f);
  Color title_collapsed = Color::FromRGBA(0.00f, 0.33f, 0.00f, 1.00f);
  Color title_active = Color::FromRGBA(0.16f, 0.65f, 0.00f, 1.00f);
  Color menu_bar = Color::FromRGBA(0.00f, 0.35f, 0.00f, 1.00f);
  /** Scrollbar track. */
  Color scrollbar = Color::FromRGBA(0.00f, 0.40f, 0.11f, 0.59f);
  /** Scrollbar thumb. */
  Color scrollbar_grab = Color::FromRGBA(0.00f, 0.68f, 0.00f, 0.68f);
  Color scrollbar_grab_hovered = Color::FromRGBA(0.00f, 1.00f, 0.15f, 0.62f);
  Color scrollbar_grab_active = Color::FromRGBA(0.00f, 0.91f, 0.09f, 0.40f);
  Color check_mark = Color::FromRGBA(0.74f, 0.90f, 0.72f, 0.50f);
  /** Slider thumb. */
  Color slider_grab = Color::FromRGBA(1.00f, 1.00f, 1.00f, 0.30f);
  Color slider_grab_active = Color::FromRGBA(0.34f, 0.75f, 0.11f, 1.00f);
  /** Selectable and tree node background. */
  Color header = Color::FromRGBA(0.00f, 0.40f, 0.00f, 0.71f);
  Color header_hovered = Color::FromRGBA(0.00f, 0.60f, 0.26f, 0.80f);
  Color header_active = Color::FromRGBA(0.00f, 0.75f, 0.00f, 0.80f);
  Color separator = Color::FromRGBA(0.00f, 0.35f, 0.00f, 1.00f);
  Color separator_hovered = Color::FromRGBA(0.36f, 0.89f, 0.38f, 1.00f);
  Color separator_active = Color::FromRGBA(0.13f, 0.50f, 0.11f, 1.00f);
  Color resize_grip = Color::FromRGBA(1.00f, 1.00f, 1.00f, 0.30f);
  Color resize_grip_hovered = Color::FromRGBA(1.00f, 1.00f, 1.00f, 0.60f);
  Color resize_grip_active = Color::FromRGBA(1.00f, 1.00f, 1.00f, 0.90f);
  Color tab = Color::FromRGBA(0.15f, 0.56f, 0.11f, 0.60f);
  Color tab_hovered = Color::FromRGBA(0.19f, 0.72f, 0.09f, 1.00f);
  Color tab_active = Color::FromRGBA(0.19f, 0.60f, 0.09f, 1.00f);
  Color tab_unfocused = Color::FromRGBA(0.80f, 0.80f, 0.80f, 0.30f);
  Color tab_unfocused_active = Color::FromRGBA(0.90f, 0.80f, 0.80f, 0.40f);
  Color plot_lines = Color::FromRGBA(1.00f, 1.00f, 1.00f, 1.00f);
  Color plot_lines_hovered = Color::FromRGBA(0.90f, 0.70f, 0.00f, 1.00f);
  /** Histogram bar. */
  Color plot_histogram = Color::FromRGBA(0.90f, 0.70f, 0.00f, 1.00f);
  Color plot_histogram_hovered = Color::FromRGBA(1.00f, 0.60f, 0.00f, 1.00f);
  /** Selected text background. */
  Color selection = Color::FromRGBA(0.00f, 1.00f, 0.00f, 0.21f);

  /** Reports whether every field is equal. */
  bool operator==(const ControlPalette& other) const = default;
};

/** The semantic color roles. A consumer override changes the theme of every ImGui slot and every
 * overlay. */
struct Palette {
  /** Buttons and the base accent. */
  Color accent = Color::FromRGBA(0.15f, 0.56f, 0.11f);
  /** Alpha of a button at rest, applied to the accent. */
  f32 button_alpha = 0.60f;
  Color accent_hovered = Color::FromRGBA(0.19f, 0.72f, 0.09f);
  Color accent_active = Color::FromRGBA(0.19f, 0.60f, 0.09f);
  /** Window background. */
  Color panel = Color::FromRGBA(0.00f, 0.06f, 0.00f);
  /** Popup and tooltip background. */
  Color panel_popup = Color::FromRGBA(0.20f, 0.20f, 0.20f, 0.99f);
  Color text = Color::FromRGBA(0.89f, 0.90f, 0.90f);
  Color text_muted = detail::kMuted;
  Color text_disabled = Color::FromRGBA(0.60f, 0.60f, 0.60f);
  Color border = Color::FromRGBA(0.00f, 0.35f, 0.00f);
  Color success = detail::kSuccess;
  Color warning = detail::kWarning;
  Color error = detail::kError;
  Color info = detail::kInfo;
  /** Emphasized values. */
  Color highlight = Color::FromRGBA(0.90f, 0.70f, 0.00f);
  /** Border shadow. */
  Color shadow = Color::FromRGBA(0.00f, 0.00f, 0.00f, 0.00f);
  /** Dim layer behind a modal window. */
  Color overlay_dim = Color::FromRGBA(0.20f, 0.20f, 0.20f, 0.35f);
  ControlPalette controls;

  /** Reports whether every field is equal. */
  bool operator==(const Palette& other) const = default;
};

/** Window background alpha per overlay kind. */
struct WindowAlpha {
  /** Full size overlays such as the achievements list. */
  f32 overlay = 0.92f;
  /** Panels such as settings and the docked console. */
  f32 panel = 0.85f;
  /** The achievement toast before its fade envelope. */
  f32 toast = 0.88f;
  /** The debug heads up display. */
  f32 hud = 0.5f;

  /** Reports whether every field is equal. */
  bool operator==(const WindowAlpha& other) const = default;
};

struct Metrics {
  f32 window_rounding = 0;
  /** Corner radius of scrollbars and tabs. */
  f32 rounding = 0;
  f32 frame_rounding = 0;
  f32 border = 1;
  Vec2 window_padding{8, 8};
  Vec2 item_spacing{8, 4};
  Vec2 frame_padding{4, 3};
  f32 icon_size = 44;
  /** Corner radius of an achievement row band. */
  f32 row_rounding = 3;
  f32 toast_rounding = 6;
  /** Gap between the toast and the screen edges. */
  f32 toast_padding = 16;
  f32 toast_width = 320;
  f32 toast_height = 70;
  /** Distance from the bottom edge the toast reserves, its height plus a margin. */
  f32 toast_stack_height = 80;
  /** Height of the bar below the settings list. */
  f32 footer_height = 30;
  /** Width of the settings category column. */
  f32 side_panel_width = 160;
  f32 button_width = 140;
  f32 value_width = 160;
  WindowAlpha window_alpha;

  bool operator==(const Metrics& other) const = default;
};

struct AchievementsStyle {
  Color unlocked_title = Color::FromRGBA(0.45f, 1.00f, 0.55f, 1.00f);
  Color unlocked_desc = Color::FromRGBA(0.70f, 0.85f, 0.72f, 1.00f);
  Color locked_title = Color::FromRGBA(0.78f, 0.80f, 0.84f, 1.00f);
  Color locked_desc = Color::FromRGBA(0.50f, 0.52f, 0.56f, 1.00f);
  Color badge_gamerscore = Color::FromRGBA(1.00f, 0.82f, 0.30f, 1.00f);
  /** Band behind an unlocked row. */
  Color row_unlocked_bg = Color::FromRGBA(0.16f, 0.30f, 0.18f, 0.55f);
  /** Summary line text. */
  Color header_text = detail::kInfo;
  Color progress_bar = Color::FromRGBA(0.30f, 0.80f, 0.40f, 1.00f);
  Color unlocked_icon_tint = colors::kWhite;
  Color locked_icon_tint = Color::FromRGBA(0.45f, 0.45f, 0.45f, 0.80f);
  Vec2 window_padding{14.0f, 12.0f};
  Vec2 item_spacing{8.0f, 6.0f};
};

/** Achievement toast colors, drawn at full opacity under the fade envelope. */
struct ToastStyle {
  /** Achievement name. */
  Color text = colors::kWhite;
  /** Heading line. */
  Color title = Color::FromRGBA(1.00f, 0.85f, 0.20f, 1.00f);
};

struct ConsoleStyle {
  /** Text color per message level, indexed by std::to_underlying(rex::log::Level). */
  std::array<Color, rex::log::kMessageLevelCount> level = {{
      Color::FromRGBA(0.5f, 0.5f, 0.5f, 1.0f),
      Color::FromRGBA(0.8f, 0.5f, 0.9f, 1.0f),
      Color::FromRGBA(0.4f, 0.9f, 0.9f, 1.0f),
      colors::kWhite,
      Color::FromRGBA(1.0f, 1.0f, 0.0f, 1.0f),
      detail::kError,
      Color::FromRGBA(1.0f, 0.0f, 0.0f, 1.0f),
  }};
  Color completion_bg = Color::FromRGBA(0.10f, 0.10f, 0.10f, 0.95f);
  Color completion_highlight = Color::FromRGBA(0.235f, 0.353f, 0.549f, 0.784f);
};

struct DebugOverlayStyle {
  /** Build stamp watermark. */
  Color muted_text = detail::kMuted;
};

struct SettingsStyle {
  /** Key conflict marker. */
  Color warning = detail::kWarning;
  /** Badge of a hot reload cvar. */
  Color lifecycle_hot_reload = detail::kSuccess;
  /** Badge of a cvar that needs a restart. */
  Color lifecycle_restart = Color::FromRGBA(1.00f, 1.00f, 0.40f, 1.00f);
  /** Badge of an init only cvar. */
  Color lifecycle_init_only = detail::kError;
  /** Badge of a cvar with no known lifecycle. */
  Color lifecycle_unknown = colors::kWhite;
};

/** Everything an overlay draws with, owned by the UI provider and patched by the consumer. */
struct Style {
  Palette palette;
  Metrics metrics;
  AchievementsStyle achievements;
  ToastStyle toast;
  ConsoleStyle console;
  DebugOverlayStyle debug;
  SettingsStyle settings;
};

}  // namespace rex::ui

namespace rex::ui {

/**
 * Converts a color to the ImGui floating point form.
 *
 * @return The color with the same four channels.
 */
inline ImVec4 ToImVec4(Color color) {
  return ImVec4(color.r, color.g, color.b, color.a);
}

/**
 * Converts a color to the packed ImGui form, red in the low byte.
 *
 * @return The color packed by Color::ToRGBA8.
 */
inline ImU32 ToImU32(Color color) {
  return color.ToRGBA8();
}

/**
 * Converts an ImGui floating point color.
 *
 * @return The color with the same four channels.
 */
inline Color FromImVec4(const ImVec4& value) {
  return Color::FromRGBA(value.x, value.y, value.z, value.w);
}

/** Sets every ImGui color slot, rounding, border and padding from the palette and metrics. */
REX_UI_API void ApplyDefaultStyle(ImGuiStyle& imgui_style, const Palette& palette,
                                  const Metrics& metrics);

}  // namespace rex::ui
