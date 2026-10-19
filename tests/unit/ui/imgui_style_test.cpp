/**
 * @file        tests/unit/ui/imgui_style_test.cpp
 * @brief       ApplyDefaultStyle on the default Palette reproduces the former ImGui slot colors
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <array>

#include <imgui.h>

#include <rex/types.h>
#include <rex/ui/color.h>
#include <rex/ui/imgui_style.h>
#include <rex/ui/style.h>

namespace {

struct Slot {
  ImGuiCol index;
  rex::f32 r;
  rex::f32 g;
  rex::f32 b;
  rex::f32 a;
};

constexpr std::array kExpected = {
    Slot{ImGuiCol_Text, 0.89f, 0.90f, 0.90f, 1.00f},
    Slot{ImGuiCol_TextDisabled, 0.60f, 0.60f, 0.60f, 1.00f},
    Slot{ImGuiCol_WindowBg, 0.00f, 0.06f, 0.00f, 1.00f},
    Slot{ImGuiCol_ChildBg, 0.00f, 0.00f, 0.00f, 0.00f},
    Slot{ImGuiCol_Border, 0.00f, 0.35f, 0.00f, 1.00f},
    Slot{ImGuiCol_BorderShadow, 0.00f, 0.00f, 0.00f, 0.00f},
    Slot{ImGuiCol_FrameBg, 0.80f, 0.80f, 0.80f, 0.30f},
    Slot{ImGuiCol_FrameBgHovered, 0.90f, 0.80f, 0.80f, 0.40f},
    Slot{ImGuiCol_FrameBgActive, 0.90f, 0.65f, 0.65f, 0.45f},
    Slot{ImGuiCol_TitleBg, 0.00f, 0.40f, 0.00f, 1.00f},
    Slot{ImGuiCol_TitleBgCollapsed, 0.00f, 0.33f, 0.00f, 1.00f},
    Slot{ImGuiCol_TitleBgActive, 0.16f, 0.65f, 0.00f, 1.00f},
    Slot{ImGuiCol_MenuBarBg, 0.00f, 0.35f, 0.00f, 1.00f},
    Slot{ImGuiCol_ScrollbarBg, 0.00f, 0.40f, 0.11f, 0.59f},
    Slot{ImGuiCol_ScrollbarGrab, 0.00f, 0.68f, 0.00f, 0.68f},
    Slot{ImGuiCol_ScrollbarGrabHovered, 0.00f, 1.00f, 0.15f, 0.62f},
    Slot{ImGuiCol_ScrollbarGrabActive, 0.00f, 0.91f, 0.09f, 0.40f},
    Slot{ImGuiCol_PopupBg, 0.20f, 0.20f, 0.20f, 0.99f},
    Slot{ImGuiCol_CheckMark, 0.74f, 0.90f, 0.72f, 0.50f},
    Slot{ImGuiCol_SliderGrab, 1.00f, 1.00f, 1.00f, 0.30f},
    Slot{ImGuiCol_SliderGrabActive, 0.34f, 0.75f, 0.11f, 1.00f},
    Slot{ImGuiCol_Button, 0.15f, 0.56f, 0.11f, 0.60f},
    Slot{ImGuiCol_ButtonHovered, 0.19f, 0.72f, 0.09f, 1.00f},
    Slot{ImGuiCol_ButtonActive, 0.19f, 0.60f, 0.09f, 1.00f},
    Slot{ImGuiCol_Header, 0.00f, 0.40f, 0.00f, 0.71f},
    Slot{ImGuiCol_HeaderHovered, 0.00f, 0.60f, 0.26f, 0.80f},
    Slot{ImGuiCol_HeaderActive, 0.00f, 0.75f, 0.00f, 0.80f},
    Slot{ImGuiCol_Separator, 0.00f, 0.35f, 0.00f, 1.00f},
    Slot{ImGuiCol_SeparatorHovered, 0.36f, 0.89f, 0.38f, 1.00f},
    Slot{ImGuiCol_SeparatorActive, 0.13f, 0.50f, 0.11f, 1.00f},
    Slot{ImGuiCol_ResizeGrip, 1.00f, 1.00f, 1.00f, 0.30f},
    Slot{ImGuiCol_ResizeGripHovered, 1.00f, 1.00f, 1.00f, 0.60f},
    Slot{ImGuiCol_ResizeGripActive, 1.00f, 1.00f, 1.00f, 0.90f},
    Slot{ImGuiCol_Tab, 0.15f, 0.56f, 0.11f, 0.60f},
    Slot{ImGuiCol_TabHovered, 0.19f, 0.72f, 0.09f, 1.00f},
    Slot{ImGuiCol_TabActive, 0.19f, 0.60f, 0.09f, 1.00f},
    Slot{ImGuiCol_TabUnfocused, 0.80f, 0.80f, 0.80f, 0.30f},
    Slot{ImGuiCol_TabUnfocusedActive, 0.90f, 0.80f, 0.80f, 0.40f},
    Slot{ImGuiCol_PlotLines, 1.00f, 1.00f, 1.00f, 1.00f},
    Slot{ImGuiCol_PlotLinesHovered, 0.90f, 0.70f, 0.00f, 1.00f},
    Slot{ImGuiCol_PlotHistogram, 0.90f, 0.70f, 0.00f, 1.00f},
    Slot{ImGuiCol_PlotHistogramHovered, 1.00f, 0.60f, 0.00f, 1.00f},
    Slot{ImGuiCol_TextSelectedBg, 0.00f, 1.00f, 0.00f, 0.21f},
    Slot{ImGuiCol_ModalWindowDimBg, 0.20f, 0.20f, 0.20f, 0.35f},
};

}  // namespace

REX_TEST_CASE("ApplyDefaultStyle on the default palette keeps every former slot color",
              "[ui][style]") {
  ImGuiStyle style;
  const rex::ui::Style defaults;
  rex::ui::ApplyDefaultStyle(style, defaults.palette, defaults.metrics);
  for (const Slot& slot : kExpected) {
    INFO("slot " << static_cast<rex::i32>(slot.index));
    CHECK(style.Colors[slot.index].x == slot.r);
    CHECK(style.Colors[slot.index].y == slot.g);
    CHECK(style.Colors[slot.index].z == slot.b);
    CHECK(style.Colors[slot.index].w == slot.a);
  }
}

REX_TEST_CASE("ApplyDefaultStyle on the default metrics keeps the ImGui sizes", "[ui][style]") {
  const ImGuiStyle reference;
  ImGuiStyle style;
  const rex::ui::Style defaults;
  rex::ui::ApplyDefaultStyle(style, defaults.palette, defaults.metrics);
  CHECK(style.WindowRounding == 0.0f);
  CHECK(style.ScrollbarRounding == 0.0f);
  CHECK(style.TabRounding == 0.0f);
  CHECK(style.FrameRounding == reference.FrameRounding);
  CHECK(style.WindowBorderSize == reference.WindowBorderSize);
  CHECK(style.WindowPadding.x == reference.WindowPadding.x);
  CHECK(style.WindowPadding.y == reference.WindowPadding.y);
  CHECK(style.ItemSpacing.x == reference.ItemSpacing.x);
  CHECK(style.ItemSpacing.y == reference.ItemSpacing.y);
  CHECK(style.FramePadding.x == reference.FramePadding.x);
  CHECK(style.FramePadding.y == reference.FramePadding.y);
}

REX_TEST_CASE("ApplyDefaultStyle re-themes the slots from an overridden palette", "[ui][style]") {
  ImGuiStyle style;
  rex::ui::Style custom;
  custom.palette.accent = rex::Color::RGBA(0.2f, 0.3f, 0.4f);
  rex::ui::ApplyDefaultStyle(style, custom.palette, custom.metrics);
  CHECK(style.Colors[ImGuiCol_Button].x == 0.2f);
  CHECK(style.Colors[ImGuiCol_Button].w == 0.60f);
}

REX_TEST_CASE("ApplyDefaultStyle takes the button alpha from the palette", "[ui][style]") {
  ImGuiStyle style;
  rex::ui::Style custom;
  custom.palette.accent = rex::Color::RGBA(0.2f, 0.3f, 0.4f);
  custom.palette.button_alpha = 1.0f;
  rex::ui::ApplyDefaultStyle(style, custom.palette, custom.metrics);
  CHECK(style.Colors[ImGuiCol_Button].x == 0.2f);
  CHECK(style.Colors[ImGuiCol_Button].w == 1.0f);
}

REX_TEST_CASE("ApplyDefaultStyle maps every control role to its slot", "[ui][style]") {
  ImGuiStyle style;
  rex::ui::Style custom;
  rex::ui::ControlPalette& controls = custom.palette.controls;
  const auto role = [](rex::i32 n) {
    return rex::Color::RGBA(static_cast<rex::f32>(n) / 32.0f, 0.5f, 0.25f, 1.0f);
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
}

REX_TEST_CASE("a changed palette reaches ImGuiStyle through ApplyDefaultStyle", "[ui][style]") {
  ImGuiStyle imgui_style;
  rex::ui::Style style;
  style.palette.accent = rex::Color::RGBA(0.7f, 0.1f, 0.3f);
  rex::ui::ApplyDefaultStyle(imgui_style, style.palette, style.metrics);
  const ImVec4 expected = rex::ui::ToImVec4(style.palette.accent.WithAlpha(0.60f));
  CHECK(imgui_style.Colors[ImGuiCol_Button].x == expected.x);
  CHECK(imgui_style.Colors[ImGuiCol_Button].y == expected.y);
  CHECK(imgui_style.Colors[ImGuiCol_Button].z == expected.z);
  CHECK(imgui_style.Colors[ImGuiCol_Button].w == expected.w);
}

REX_TEST_CASE("metrics rounding reaches the ImGui roundings", "[ui][style]") {
  ImGuiStyle imgui_style;
  rex::ui::Style style;
  style.metrics.window_rounding = 7.0f;
  style.metrics.rounding = 5.0f;
  style.metrics.frame_rounding = 2.0f;
  rex::ui::ApplyDefaultStyle(imgui_style, style.palette, style.metrics);
  CHECK(imgui_style.WindowRounding == 7.0f);
  CHECK(imgui_style.ScrollbarRounding == 5.0f);
  CHECK(imgui_style.TabRounding == 5.0f);
  CHECK(imgui_style.FrameRounding == 2.0f);
}
