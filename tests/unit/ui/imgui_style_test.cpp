/**
 * @file        tests/unit/ui/imgui_style_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <imgui.h>

#include <rex/types.h>
#include <rex/ui/style.h>

REX_TEST_CASE("ApplyDefaultStyle takes the button color from the palette accent and alpha",
              "[ui][style]") {
  ImGuiStyle imgui_style;
  rex::ui::Style style;
  style.palette.accent = rex::ui::Color::FromRGBA(0.7f, 0.1f, 0.3f);
  rex::ui::ApplyDefaultStyle(imgui_style, style.palette, style.metrics);
  const ImVec4 expected = rex::ui::ToImVec4(style.palette.accent.WithAlpha(0.60f));
  CHECK(imgui_style.Colors[ImGuiCol_Button].x == expected.x);
  CHECK(imgui_style.Colors[ImGuiCol_Button].y == expected.y);
  CHECK(imgui_style.Colors[ImGuiCol_Button].z == expected.z);
  CHECK(imgui_style.Colors[ImGuiCol_Button].w == 0.60f);

  SECTION("A button alpha of one is kept") {
    style.palette.button_alpha = 1.0f;
    rex::ui::ApplyDefaultStyle(imgui_style, style.palette, style.metrics);
    CHECK(imgui_style.Colors[ImGuiCol_Button].x == expected.x);
    CHECK(imgui_style.Colors[ImGuiCol_Button].w == 1.0f);
  }
}

REX_TEST_CASE("ApplyDefaultStyle maps every control role to its slot", "[ui][style]") {
  ImGuiStyle style;
  rex::ui::Style custom;
  rex::ui::ControlPalette& controls = custom.palette.controls;
  const auto role = [](rex::i32 n) {
    return rex::ui::Color::FromRGBA(static_cast<rex::f32>(n) / 32.0f, 0.5f, 0.25f, 1.0f);
  };
  controls.menu_bar = role(1);
  controls.separator = role(2);
  controls.slider_grab = role(3);
  controls.resize_grip = role(4);
  controls.resize_grip_hovered = role(5);
  controls.resize_grip_active = role(6);
  controls.tab = role(7);
  controls.tab_hovered = role(8);
  controls.tab_active = role(9);
  controls.tab_unfocused = role(10);
  controls.tab_unfocused_active = role(11);
  controls.header = role(12);
  controls.plot_lines = role(13);
  controls.plot_lines_hovered = role(14);
  controls.plot_histogram = role(15);
  rex::ui::ApplyDefaultStyle(style, custom.palette, custom.metrics);
  const ImGuiCol slots[] = {
      ImGuiCol_MenuBarBg,    ImGuiCol_Separator,          ImGuiCol_SliderGrab,
      ImGuiCol_ResizeGrip,   ImGuiCol_ResizeGripHovered,  ImGuiCol_ResizeGripActive,
      ImGuiCol_Tab,          ImGuiCol_TabHovered,         ImGuiCol_TabActive,
      ImGuiCol_TabUnfocused, ImGuiCol_TabUnfocusedActive, ImGuiCol_Header,
      ImGuiCol_PlotLines,    ImGuiCol_PlotLinesHovered,   ImGuiCol_PlotHistogram,
  };
  for (rex::i32 i = 0; i < 15; ++i) {
    INFO("slot " << static_cast<rex::i32>(slots[i]));
    CHECK(style.Colors[slots[i]].x == role(i + 1).r);
  }

  ImGuiStyle imgui_style;
  rex::ui::Style table_custom;
  rex::ui::ControlPalette& table_controls = table_custom.palette.controls;
  table_controls.table_header = role(1);
  table_controls.table_border_strong = role(2);
  table_controls.table_border_light = role(3);
  table_controls.table_row = role(4);
  table_controls.table_row_alt = role(5);
  table_controls.text_link = role(6);
  table_controls.tree_lines = role(7);
  table_controls.nav_cursor = role(8);
  table_controls.nav_windowing_highlight = role(9);
  table_controls.nav_windowing_dim = role(10);
  table_custom.metrics.child_rounding = 2.0f;
  table_custom.metrics.popup_rounding = 3.0f;
  table_custom.metrics.grab_rounding = 4.0f;
  rex::ui::ApplyDefaultStyle(imgui_style, table_custom.palette, table_custom.metrics);
  const ImGuiCol table_slots[] = {
      ImGuiCol_TableHeaderBg,     ImGuiCol_TableBorderStrong, ImGuiCol_TableBorderLight,
      ImGuiCol_TableRowBg,        ImGuiCol_TableRowBgAlt,     ImGuiCol_TextLink,
      ImGuiCol_TreeLines,         ImGuiCol_NavCursor,         ImGuiCol_NavWindowingHighlight,
      ImGuiCol_NavWindowingDimBg,
  };
  for (rex::i32 i = 0; i < 10; ++i) {
    INFO("slot " << static_cast<rex::i32>(table_slots[i]));
    CHECK(imgui_style.Colors[table_slots[i]].x == role(i + 1).r);
  }
  CHECK(imgui_style.ChildRounding == 2.0f);
  CHECK(imgui_style.PopupRounding == 3.0f);
  CHECK(imgui_style.GrabRounding == 4.0f);
}

REX_TEST_CASE("metrics rounding reaches the ImGui roundings", "[ui][style]") {
  ImGuiStyle imgui_style;
  rex::ui::Style style;
  rex::ui::ApplyDefaultStyle(imgui_style, style.palette, style.metrics);
  CHECK(imgui_style.WindowRounding == 0.0f);
  CHECK(imgui_style.ScrollbarRounding == 0.0f);
  CHECK(imgui_style.TabRounding == 0.0f);

  style.metrics.window_rounding = 7.0f;
  style.metrics.rounding = 5.0f;
  style.metrics.frame_rounding = 2.0f;
  rex::ui::ApplyDefaultStyle(imgui_style, style.palette, style.metrics);
  CHECK(imgui_style.WindowRounding == 7.0f);
  CHECK(imgui_style.ScrollbarRounding == 5.0f);
  CHECK(imgui_style.TabRounding == 5.0f);
  CHECK(imgui_style.FrameRounding == 2.0f);
}
