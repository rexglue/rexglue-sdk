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

#include <rex/color.h>
#include <rex/logging/level.h>
#include <rex/types.h>

namespace rex::ui {

namespace detail {

inline constexpr Color kSuccess = Color::RGBA(0.40f, 1.00f, 0.40f);
inline constexpr Color kWarning = Color::RGBA(1.00f, 0.80f, 0.20f);
inline constexpr Color kError = Color::RGBA(1.00f, 0.40f, 0.40f);
inline constexpr Color kInfo = Color::RGBA(0.60f, 0.85f, 1.00f);
inline constexpr Color kMuted = Color::RGBA(1.00f, 1.00f, 1.00f, 0.50f);

}  // namespace detail

/** A 2 component size or offset in pixels. */
struct Vec2 {
  /** Horizontal component. */
  f32 x = 0;
  /** Vertical component. */
  f32 y = 0;

  /**
   * Reports whether every field is equal.
   *
   * @param other The value to compare with.
   * @return True when every field matches exactly.
   */
  constexpr bool operator==(const Vec2& other) const = default;
};

/** Widget colors with no semantic role, mapped 1 to 1 onto ImGui slots. */
struct ControlPalette {
  /** Input frame background. */
  Color frame = Color::RGBA(0.80f, 0.80f, 0.80f, 0.30f);
  /** Input frame background under the pointer. */
  Color frame_hovered = Color::RGBA(0.90f, 0.80f, 0.80f, 0.40f);
  /** Input frame background while pressed. */
  Color frame_active = Color::RGBA(0.90f, 0.65f, 0.65f, 0.45f);
  /** Window title bar and the base of the header colors. */
  Color title = Color::RGBA(0.00f, 0.40f, 0.00f, 1.00f);
  /** Collapsed window title bar. */
  Color title_collapsed = Color::RGBA(0.00f, 0.33f, 0.00f, 1.00f);
  /** Focused window title bar. */
  Color title_active = Color::RGBA(0.16f, 0.65f, 0.00f, 1.00f);
  /** Menu bar background. */
  Color menu_bar = Color::RGBA(0.00f, 0.35f, 0.00f, 1.00f);
  /** Scrollbar track. */
  Color scrollbar = Color::RGBA(0.00f, 0.40f, 0.11f, 0.59f);
  /** Scrollbar thumb. */
  Color scrollbar_grab = Color::RGBA(0.00f, 0.68f, 0.00f, 0.68f);
  /** Scrollbar thumb under the pointer. */
  Color scrollbar_grab_hovered = Color::RGBA(0.00f, 1.00f, 0.15f, 0.62f);
  /** Scrollbar thumb while dragged. */
  Color scrollbar_grab_active = Color::RGBA(0.00f, 0.91f, 0.09f, 0.40f);
  /** Check mark of a checkbox. */
  Color check_mark = Color::RGBA(0.74f, 0.90f, 0.72f, 0.50f);
  /** Slider thumb. */
  Color slider_grab = Color::RGBA(1.00f, 1.00f, 1.00f, 0.30f);
  /** Slider thumb while dragged. */
  Color slider_grab_active = Color::RGBA(0.34f, 0.75f, 0.11f, 1.00f);
  /** Selectable and tree node. */
  Color header = Color::RGBA(0.00f, 0.40f, 0.00f, 0.71f);
  /** Selectable and tree node under the pointer. */
  Color header_hovered = Color::RGBA(0.00f, 0.60f, 0.26f, 0.80f);
  /** Selectable and tree node while pressed. */
  Color header_active = Color::RGBA(0.00f, 0.75f, 0.00f, 0.80f);
  /** Separator line. */
  Color separator = Color::RGBA(0.00f, 0.35f, 0.00f, 1.00f);
  /** Separator under the pointer. */
  Color separator_hovered = Color::RGBA(0.36f, 0.89f, 0.38f, 1.00f);
  /** Separator while dragged. */
  Color separator_active = Color::RGBA(0.13f, 0.50f, 0.11f, 1.00f);
  /** Window resize grip. */
  Color resize_grip = Color::RGBA(1.00f, 1.00f, 1.00f, 0.30f);
  /** Window resize grip under the pointer. */
  Color resize_grip_hovered = Color::RGBA(1.00f, 1.00f, 1.00f, 0.60f);
  /** Window resize grip while dragged. */
  Color resize_grip_active = Color::RGBA(1.00f, 1.00f, 1.00f, 0.90f);
  /** Tab. */
  Color tab = Color::RGBA(0.15f, 0.56f, 0.11f, 0.60f);
  /** Tab under the pointer. */
  Color tab_hovered = Color::RGBA(0.19f, 0.72f, 0.09f, 1.00f);
  /** Selected tab. */
  Color tab_active = Color::RGBA(0.19f, 0.60f, 0.09f, 1.00f);
  /** Tab of an unfocused window. */
  Color tab_unfocused = Color::RGBA(0.80f, 0.80f, 0.80f, 0.30f);
  /** Selected tab of an unfocused window. */
  Color tab_unfocused_active = Color::RGBA(0.90f, 0.80f, 0.80f, 0.40f);
  /** Plot line. */
  Color plot_lines = Color::RGBA(1.00f, 1.00f, 1.00f, 1.00f);
  /** Plot line under the pointer. */
  Color plot_lines_hovered = Color::RGBA(0.90f, 0.70f, 0.00f, 1.00f);
  /** Histogram bar. */
  Color plot_histogram = Color::RGBA(0.90f, 0.70f, 0.00f, 1.00f);
  /** Histogram bar under the pointer. */
  Color plot_histogram_hovered = Color::RGBA(1.00f, 0.60f, 0.00f, 1.00f);
  /** Selected text background. */
  Color selection = Color::RGBA(0.00f, 1.00f, 0.00f, 0.21f);

  /**
   * Reports whether every field is equal.
   *
   * @param other The value to compare with.
   * @return True when every field matches exactly.
   */
  bool operator==(const ControlPalette& other) const = default;
};

/** The semantic color roles. A consumer override re-themes every ImGui slot and every overlay. */
struct Palette {
  /** Buttons and the base accent. */
  Color accent = Color::RGBA(0.15f, 0.56f, 0.11f);
  /** Alpha of a button at rest, applied to the accent. */
  f32 button_alpha = 0.60f;
  /** Accent under the pointer. */
  Color accent_hovered = Color::RGBA(0.19f, 0.72f, 0.09f);
  /** Accent while pressed. */
  Color accent_active = Color::RGBA(0.19f, 0.60f, 0.09f);
  /** Window background. */
  Color panel = Color::RGBA(0.00f, 0.06f, 0.00f);
  /** Popup and tooltip background. */
  Color panel_popup = Color::RGBA(0.20f, 0.20f, 0.20f, 0.99f);
  /** Body text. */
  Color text = Color::RGBA(0.89f, 0.90f, 0.90f);
  /** De-emphasized text. */
  Color text_muted = detail::kMuted;
  /** Text of a disabled widget. */
  Color text_disabled = Color::RGBA(0.60f, 0.60f, 0.60f);
  /** Window borders. */
  Color border = Color::RGBA(0.00f, 0.35f, 0.00f);
  /** Success state. */
  Color success = detail::kSuccess;
  /** Warning state. */
  Color warning = detail::kWarning;
  /** Error state. */
  Color error = detail::kError;
  /** Informational text. */
  Color info = detail::kInfo;
  /** Emphasized values. */
  Color highlight = Color::RGBA(0.90f, 0.70f, 0.00f);
  /** Border shadow. */
  Color shadow = Color::RGBA(0.00f, 0.00f, 0.00f, 0.00f);
  /** Dim layer behind a modal window. */
  Color overlay_dim = Color::RGBA(0.20f, 0.20f, 0.20f, 0.35f);
  /** Widget colors without a semantic role. */
  ControlPalette controls;

  /**
   * Reports whether every field is equal.
   *
   * @param other The value to compare with.
   * @return True when every field matches exactly.
   */
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

  /**
   * Reports whether every field is equal.
   *
   * @param other The value to compare with.
   * @return True when every field matches exactly.
   */
  bool operator==(const WindowAlpha& other) const = default;
};

/** Sizes shared by every overlay and by the ImGui style. */
struct Metrics {
  /** Corner radius of windows. */
  f32 window_rounding = 0;
  /** Corner radius of scrollbars and tabs. */
  f32 rounding = 0;
  /** Corner radius of input frames. */
  f32 frame_rounding = 0;
  /** Window border thickness. */
  f32 border = 1;
  /** Padding inside a window. */
  Vec2 window_padding{8, 8};
  /** Spacing between widgets. */
  Vec2 item_spacing{8, 4};
  /** Padding inside an input frame. */
  Vec2 frame_padding{4, 3};
  /** Edge of an achievement icon. */
  f32 icon_size = 44;
  /** Corner radius of an achievement row band. */
  f32 row_rounding = 3;
  /** Corner radius of the toast window. */
  f32 toast_rounding = 6;
  /** Gap between the toast and the screen edges. */
  f32 toast_padding = 16;
  /** Width of the toast window. */
  f32 toast_width = 320;
  /** Height of the toast window. */
  f32 toast_height = 70;
  /** Distance from the bottom screen edge that the toast window reserves, its height plus a margin.
   */
  f32 toast_stack_height = 80;
  /** Height of the bar below the settings list. */
  f32 footer_height = 30;
  /** Width of the settings category column. */
  f32 side_panel_width = 160;
  /** Width of a standard button. */
  f32 button_width = 140;
  /** Width of a value widget. */
  f32 button_width_wide = 160;
  /** Background alpha per overlay kind. */
  WindowAlpha window_alpha;

  /**
   * Reports whether every field is equal.
   *
   * @param other The value to compare with.
   * @return True when every field matches exactly.
   */
  bool operator==(const Metrics& other) const = default;
};

/** Achievements overlay values the palette lacks. */
struct AchievementsStyle {
  /** Title of an unlocked achievement. */
  Color unlocked_title = Color::RGBA(0.45f, 1.00f, 0.55f, 1.00f);
  /** Description of an unlocked achievement. */
  Color unlocked_desc = Color::RGBA(0.70f, 0.85f, 0.72f, 1.00f);
  /** Title of a locked achievement. */
  Color locked_title = Color::RGBA(0.78f, 0.80f, 0.84f, 1.00f);
  /** Description of a locked achievement. */
  Color locked_desc = Color::RGBA(0.50f, 0.52f, 0.56f, 1.00f);
  /** Gamerscore badge. */
  Color badge_gamerscore = Color::RGBA(1.00f, 0.82f, 0.30f, 1.00f);
  /** Band behind an unlocked row. */
  Color row_unlocked_bg = Color::RGBA(0.16f, 0.30f, 0.18f, 0.55f);
  /** Summary line text. */
  Color header_text = detail::kInfo;
  /** Progress bar fill. */
  Color progress_bar = Color::RGBA(0.30f, 0.80f, 0.40f, 1.00f);
  /** Tint of an unlocked icon. */
  Color unlocked_icon_tint = colors::kWhite;
  /** Tint of a locked icon. */
  Color locked_icon_tint = Color::RGBA(0.45f, 0.45f, 0.45f, 0.80f);
  /** Padding inside the overlay window. */
  Vec2 window_padding{14.0f, 12.0f};
  /** Spacing between widgets in the overlay window. */
  Vec2 item_spacing{8.0f, 6.0f};
};

/** Achievement toast colors, drawn at full opacity under the fade envelope. */
struct ToastStyle {
  /** Achievement name. */
  Color text = colors::kWhite;
  /** Heading line. */
  Color title = Color::RGBA(1.00f, 0.85f, 0.20f, 1.00f);
};

/** Console colors. */
struct ConsoleStyle {
  /** Text color per message level, indexed by std::to_underlying(rex::log::Level). */
  std::array<Color, rex::log::kMessageLevelCount> level = {{
      Color::RGBA(0.5f, 0.5f, 0.5f, 1.0f),
      Color::RGBA(0.8f, 0.5f, 0.9f, 1.0f),
      Color::RGBA(0.4f, 0.9f, 0.9f, 1.0f),
      colors::kWhite,
      Color::RGBA(1.0f, 1.0f, 0.0f, 1.0f),
      detail::kError,
      Color::RGBA(1.0f, 0.0f, 0.0f, 1.0f),
  }};
  /** Completion popup background. */
  Color completion_bg = Color::RGBA(0.10f, 0.10f, 0.10f, 0.95f);
  /** Highlighted completion row. */
  Color completion_highlight = Color::RGBA(0.235f, 0.353f, 0.549f, 0.784f);
};

/** Debug overlay colors. */
struct DebugOverlayStyle {
  /** Build stamp watermark. */
  Color muted_text = detail::kMuted;
};

/** Settings overlay colors. */
struct SettingsStyle {
  /** Key conflict marker. */
  Color warning = detail::kWarning;
  /** Badge of a live cvar. */
  Color lifecycle_live = detail::kSuccess;
  /** Badge of a cvar that needs a restart. */
  Color lifecycle_restart = Color::RGBA(1.00f, 1.00f, 0.40f, 1.00f);
  /** Badge of an init only cvar. */
  Color lifecycle_init_only = detail::kError;
  /** Badge of a cvar with no known lifecycle. */
  Color lifecycle_unknown = colors::kWhite;
};

/** Everything an overlay draws with, owned by the ui service and patched by the consumer. */
struct Style {
  /** The color roles. */
  Palette palette;
  /** The sizes. */
  Metrics metrics;
  /** Achievements overlay values. */
  AchievementsStyle achievements;
  /** Achievement toast values. */
  ToastStyle toast;
  /** Console values. */
  ConsoleStyle console;
  /** Debug overlay values. */
  DebugOverlayStyle debug;
  /** Settings overlay values. */
  SettingsStyle settings;
};

}  // namespace rex::ui
