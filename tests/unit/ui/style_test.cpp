/**
 * @file        tests/unit/ui/style_test.cpp
 * @brief       The default Palette and Metrics hold the overlay values
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/color.h>
#include <rex/ui/style.h>

using rex::Color;
using rex::ui::Style;

REX_TEST_CASE("Default Metrics hold the former overlay literals", "[ui][style]") {
  const Style style;
  const auto& metrics = style.metrics;
  CHECK(metrics.window_rounding == 0.0f);
  CHECK(metrics.rounding == 0.0f);
  CHECK(metrics.frame_rounding == 0.0f);
  CHECK(metrics.border == 1.0f);
  CHECK(metrics.window_padding.x == 8.0f);
  CHECK(metrics.window_padding.y == 8.0f);
  CHECK(metrics.item_spacing.x == 8.0f);
  CHECK(metrics.item_spacing.y == 4.0f);
  CHECK(metrics.frame_padding.x == 4.0f);
  CHECK(metrics.frame_padding.y == 3.0f);
  CHECK(metrics.icon_size == 44.0f);
  CHECK(metrics.row_rounding == 3.0f);
  CHECK(metrics.toast_rounding == 6.0f);
  CHECK(metrics.toast_padding == 16.0f);
  CHECK(metrics.toast_width == 320.0f);
  CHECK(metrics.toast_height == 70.0f);
  CHECK(metrics.toast_stack_height == 80.0f);
  CHECK(metrics.footer_height == 30.0f);
  CHECK(metrics.side_panel_width == 160.0f);
  CHECK(metrics.button_width == 140.0f);
  CHECK(metrics.value_width == 160.0f);
  CHECK(metrics.window_alpha.overlay == 0.92f);
  CHECK(metrics.window_alpha.panel == 0.85f);
  CHECK(metrics.window_alpha.toast == 0.88f);
  CHECK(metrics.window_alpha.hud == 0.5f);
}

REX_TEST_CASE("Default section colors reference the palette roles they equal", "[ui][style]") {
  const Style style;
  CHECK(style.settings.warning == style.palette.warning);
  CHECK(style.settings.lifecycle_hot_reload == style.palette.success);
  CHECK(style.settings.lifecycle_init_only == style.palette.error);
  CHECK(style.achievements.header_text == style.palette.info);
  CHECK(style.debug.muted_text == style.palette.text_muted);
  CHECK(style.console.level[5] == style.palette.error);
  CHECK(style.toast.text == rex::colors::kWhite);
  CHECK(style.settings.lifecycle_unknown == rex::colors::kWhite);
}

REX_TEST_CASE("Default section colors keep the former values", "[ui][style]") {
  const Style style;
  CHECK(style.achievements.unlocked_title == Color::FromRGBA(0.45f, 1.00f, 0.55f, 1.00f));
  CHECK(style.achievements.row_unlocked_bg == Color::FromRGBA(0.16f, 0.30f, 0.18f, 0.55f));
  CHECK(style.achievements.locked_icon_tint == Color::FromRGBA(0.45f, 0.45f, 0.45f, 0.80f));
  CHECK(style.achievements.window_padding.x == 14.0f);
  CHECK(style.achievements.window_padding.y == 12.0f);
  CHECK(style.achievements.item_spacing.x == 8.0f);
  CHECK(style.achievements.item_spacing.y == 6.0f);
  CHECK(style.toast.title == Color::FromRGBA(1.00f, 0.85f, 0.20f, 1.00f));
  CHECK(style.console.level[0] == Color::FromRGBA(0.5f, 0.5f, 0.5f, 1.0f));
  CHECK(style.console.level[6] == Color::FromRGBA(1.0f, 0.0f, 0.0f, 1.0f));
  CHECK(style.console.completion_bg == Color::FromRGBA(0.10f, 0.10f, 0.10f, 0.95f));
  CHECK(style.console.completion_highlight == Color::FromRGBA(0.235f, 0.353f, 0.549f, 0.784f));
  CHECK(style.palette.button_alpha == 0.60f);
  CHECK(style.settings.lifecycle_restart == Color::FromRGBA(1.00f, 1.00f, 0.40f, 1.00f));
}
