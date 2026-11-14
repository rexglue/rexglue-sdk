#include <rex/ui/style.h>

namespace rex::ui {

void ApplyDefaultStyle(ImGuiStyle& imgui_style, const Palette& palette, const Metrics& metrics) {
  const ControlPalette& controls = palette.controls;
  auto set = [&imgui_style](ImGuiCol slot, Color color) {
    imgui_style.Colors[slot] = ToImVec4(color);
  };

  imgui_style.WindowRounding = metrics.window_rounding;
  imgui_style.ScrollbarRounding = metrics.rounding;
  imgui_style.TabRounding = metrics.rounding;
  imgui_style.FrameRounding = metrics.frame_rounding;
  imgui_style.ChildRounding = metrics.child_rounding;
  imgui_style.PopupRounding = metrics.popup_rounding;
  imgui_style.GrabRounding = metrics.grab_rounding;
  imgui_style.WindowBorderSize = metrics.border;
  imgui_style.WindowPadding = ImVec2(metrics.window_padding.x, metrics.window_padding.y);
  imgui_style.ItemSpacing = ImVec2(metrics.item_spacing.x, metrics.item_spacing.y);
  imgui_style.FramePadding = ImVec2(metrics.frame_padding.x, metrics.frame_padding.y);

  set(ImGuiCol_Text, palette.text);
  set(ImGuiCol_TextDisabled, palette.text_disabled);
  set(ImGuiCol_WindowBg, palette.panel);
  set(ImGuiCol_ChildBg, colors::kTransparent);
  set(ImGuiCol_Border, palette.border);
  set(ImGuiCol_BorderShadow, palette.shadow);
  set(ImGuiCol_FrameBg, controls.frame);
  set(ImGuiCol_FrameBgHovered, controls.frame_hovered);
  set(ImGuiCol_FrameBgActive, controls.frame_active);
  set(ImGuiCol_TitleBg, controls.title);
  set(ImGuiCol_TitleBgCollapsed, controls.title_collapsed);
  set(ImGuiCol_TitleBgActive, controls.title_active);
  set(ImGuiCol_MenuBarBg, controls.menu_bar);
  set(ImGuiCol_ScrollbarBg, controls.scrollbar);
  set(ImGuiCol_ScrollbarGrab, controls.scrollbar_grab);
  set(ImGuiCol_ScrollbarGrabHovered, controls.scrollbar_grab_hovered);
  set(ImGuiCol_ScrollbarGrabActive, controls.scrollbar_grab_active);
  set(ImGuiCol_PopupBg, palette.panel_popup);
  set(ImGuiCol_CheckMark, controls.check_mark);
  set(ImGuiCol_SliderGrab, controls.slider_grab);
  set(ImGuiCol_SliderGrabActive, controls.slider_grab_active);
  set(ImGuiCol_Button, palette.accent.WithAlpha(palette.button_alpha));
  set(ImGuiCol_ButtonHovered, palette.accent_hovered);
  set(ImGuiCol_ButtonActive, palette.accent_active);
  set(ImGuiCol_Header, controls.header);
  set(ImGuiCol_HeaderHovered, controls.header_hovered);
  set(ImGuiCol_HeaderActive, controls.header_active);
  set(ImGuiCol_Separator, controls.separator);
  set(ImGuiCol_SeparatorHovered, controls.separator_hovered);
  set(ImGuiCol_SeparatorActive, controls.separator_active);
  set(ImGuiCol_ResizeGrip, controls.resize_grip);
  set(ImGuiCol_ResizeGripHovered, controls.resize_grip_hovered);
  set(ImGuiCol_ResizeGripActive, controls.resize_grip_active);
  set(ImGuiCol_Tab, controls.tab);
  set(ImGuiCol_TabHovered, controls.tab_hovered);
  set(ImGuiCol_TabActive, controls.tab_active);
  set(ImGuiCol_TabUnfocused, controls.tab_unfocused);
  set(ImGuiCol_TabUnfocusedActive, controls.tab_unfocused_active);
  set(ImGuiCol_PlotLines, controls.plot_lines);
  set(ImGuiCol_PlotLinesHovered, controls.plot_lines_hovered);
  set(ImGuiCol_PlotHistogram, controls.plot_histogram);
  set(ImGuiCol_PlotHistogramHovered, controls.plot_histogram_hovered);
  set(ImGuiCol_TextSelectedBg, controls.selection);
  set(ImGuiCol_TableHeaderBg, controls.table_header);
  set(ImGuiCol_TableBorderStrong, controls.table_border_strong);
  set(ImGuiCol_TableBorderLight, controls.table_border_light);
  set(ImGuiCol_TableRowBg, controls.table_row);
  set(ImGuiCol_TableRowBgAlt, controls.table_row_alt);
  set(ImGuiCol_TextLink, controls.text_link);
  set(ImGuiCol_TreeLines, controls.tree_lines);
  set(ImGuiCol_NavCursor, controls.nav_cursor);
  set(ImGuiCol_NavWindowingHighlight, controls.nav_windowing_highlight);
  set(ImGuiCol_NavWindowingDimBg, controls.nav_windowing_dim);
  set(ImGuiCol_ModalWindowDimBg, palette.overlay_dim);
}

}  // namespace rex::ui
