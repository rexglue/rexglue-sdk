/**
 * @file        rex/ui/style.h
 *
 * @brief       Per-overlay styling that ImGuiStyle cannot express.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     ImGuiDrawer owns one Style; overlays read their own section
 *              through ImGuiDialog::imgui_drawer()->style(). Consumers patch
 *              it through TitleUI::theme in OnCreateWindow. Every default matches the
 *              value the corresponding overlay used before the theme existed.
 */
#pragma once

#include <array>

#include <imgui.h>

#include <rex/logging/level.h>

namespace rex::ui {

struct AchievementsStyle {
  ImVec4 unlocked_title{0.45f, 1.00f, 0.55f, 1.00f};
  ImVec4 unlocked_desc{0.70f, 0.85f, 0.72f, 1.00f};
  ImVec4 locked_title{0.78f, 0.80f, 0.84f, 1.00f};
  ImVec4 locked_desc{0.50f, 0.52f, 0.56f, 1.00f};
  ImVec4 badge_gamerscore{1.00f, 0.82f, 0.30f, 1.00f};
  ImVec4 row_unlocked_bg{0.16f, 0.30f, 0.18f, 0.55f};
  ImVec4 header_text{0.60f, 0.85f, 1.00f, 1.00f};
  ImVec4 progress_bar{0.30f, 0.80f, 0.40f, 1.00f};
  ImVec4 unlocked_icon_tint{1.00f, 1.00f, 1.00f, 1.00f};
  ImVec4 locked_icon_tint{0.45f, 0.45f, 0.45f, 0.80f};
  ImVec2 window_padding{14.0f, 12.0f};
  ImVec2 item_spacing{8.0f, 6.0f};
  float icon_size = 44.0f;
  float row_rounding = 3.0f;
};

struct ToastStyle {
  /// Alpha is the fade envelope, applied on top of these at draw time.
  ImVec4 text{1.00f, 1.00f, 1.00f, 1.00f};
  ImVec4 title{1.00f, 0.85f, 0.20f, 1.00f};
  float rounding = 6.0f;
  float background_alpha = 0.88f;
  float icon_size = 44.0f;
};

struct ConsoleStyle {
  /** Text color per message level, indexed by std::to_underlying(rex::log::Level). */
  std::array<ImVec4, rex::log::kMessageLevelCount> level = {{
      {0.5f, 0.5f, 0.5f, 1.0f},
      {0.8f, 0.5f, 0.9f, 1.0f},
      {0.4f, 0.9f, 0.9f, 1.0f},
      {1.0f, 1.0f, 1.0f, 1.0f},
      {1.0f, 1.0f, 0.0f, 1.0f},
      {1.0f, 0.4f, 0.4f, 1.0f},
      {1.0f, 0.0f, 0.0f, 1.0f},
  }};
  ImVec4 completion_bg{0.10f, 0.10f, 0.10f, 0.95f};
  ImVec4 completion_highlight{0.235f, 0.353f, 0.549f, 0.784f};
};

struct DebugOverlayStyle {
  ImVec4 muted_text{1.00f, 1.00f, 1.00f, 0.50f};
};

struct SettingsStyle {
  ImVec4 warning{1.00f, 0.80f, 0.20f, 1.00f};
  /// Cvar lifecycle badges: [live], [restart], [init-only].
  ImVec4 lifecycle_live{0.40f, 1.00f, 0.40f, 1.00f};
  ImVec4 lifecycle_restart{1.00f, 1.00f, 0.40f, 1.00f};
  ImVec4 lifecycle_init_only{1.00f, 0.40f, 0.40f, 1.00f};
  ImVec4 lifecycle_unknown{1.00f, 1.00f, 1.00f, 1.00f};
};

struct Style {
  AchievementsStyle achievements;
  ToastStyle toast;
  ConsoleStyle console;
  DebugOverlayStyle debug;
  SettingsStyle settings;
};

}  // namespace rex::ui
