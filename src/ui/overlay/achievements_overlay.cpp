/**
 * @file        ui/overlay/achievements_overlay.cpp
 * @brief       Achievements overlay implementation. See achievements_overlay.h for details.
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/achievements_overlay.h>
#include <rex/ui/overlay/window_chrome.h>

#include <imgui.h>

#include <rex/runtime.h>
#include <rex/ui/immediate_drawer.h>

#include "achievement_groups.h"

namespace rex::ui {

AchievementsOverlayDialog::AchievementsOverlayDialog(ImGuiDrawer* imgui_drawer,
                                                     ImmediateDrawer* immediate_drawer,
                                                     rex::Runtime* runtime,
                                                     rex::system::AchievementManager* achievements,
                                                     std::function<void()> close_callback)
    : ImGuiDialog(imgui_drawer),
      achievements_(achievements),
      icon_cache_(immediate_drawer, runtime),
      close_callback_(std::move(close_callback)) {}

AchievementsOverlayDialog::~AchievementsOverlayDialog() {}

ImmediateTexture* AchievementsOverlayDialog::GetIcon(
    const rex::system::AchievementInfo& achievement) {
  return icon_cache_.GetIcon(achievement);
}

void AchievementsOverlayDialog::OnDraw(ImGuiIO& io) {
  const AchievementsStyle& style = imgui_drawer()->style().achievements;

  const float scale = imgui_drawer()->overlay_scale();
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 40.0f * scale), ImGuiCond_FirstUseEver,
                          ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowSize(ImVec2(640.0f * scale, 560.0f * scale), ImGuiCond_FirstUseEver);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                      ImVec2(style.window_padding.x * scale, style.window_padding.y * scale));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                      ImVec2(style.item_spacing.x * scale, style.item_spacing.y * scale));

  bool open = true;
  if (BeginOverlayWindow(imgui_drawer()->style(), "Achievements##overlay",
                         close_callback_ ? &open : nullptr, ImGuiWindowFlags_NoCollapse, 0.92f)) {
    if (close_callback_ && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      open = false;
    }
    const auto groups = achievement_groups::Split(achievements_->ListAchievements());

    const auto draw_group = [&](const std::vector<rex::system::AchievementInfo>& achievements) {
      const auto totals = achievement_groups::Summarize(
          achievements, [this](uint32_t id) { return achievements_->IsUnlocked(id); });
      const int unlocked_count = totals.unlocked;
      const int total_count = totals.total;
      const int earned_gs = totals.earned_gamerscore;
      const int total_gs = totals.total_gamerscore;

      // ---- Header: summary line + progress bar -------------------------------
      ImGui::PushStyleColor(ImGuiCol_Text, style.header_text);
      ImGui::Text("%d / %d unlocked", unlocked_count, total_count);
      ImGui::PopStyleColor();
      ImGui::SameLine();
      ImGui::TextColored(style.badge_gamerscore, "%dG / %dG", earned_gs, total_gs);

      float frac = total_count > 0 ? static_cast<float>(unlocked_count) / total_count : 0.0f;
      ImGui::PushStyleColor(ImGuiCol_PlotHistogram, style.progress_bar);
      ImGui::ProgressBar(frac, ImVec2(-1.0f, 6.0f * scale), "");
      ImGui::PopStyleColor();

      ImGui::TextDisabled("Unlock progress is saved automatically");
      ImGui::Separator();
      ImGui::Spacing();

      // ---- List --------------------------------------------------------------
      ImGui::BeginChild("##achlist", ImVec2(0.0f, 0.0f), false);
      ImDrawList* draw_list = ImGui::GetWindowDrawList();

      for (const auto& a : achievements) {
        const bool is_unlocked = achievements_->IsUnlocked(a.id);
        const std::string& desc = is_unlocked ? a.description : a.unachieved_description;

        ImGui::PushID(static_cast<int>(a.id));

        // Split the draw list so the row band (channel 0) renders behind the
        // text (channel 1); we paint the rect after measuring, then merge.
        draw_list->ChannelsSplit(2);
        draw_list->ChannelsSetCurrent(1);

        const float row_start_y = ImGui::GetCursorScreenPos().y;
        const float pad = 4.0f;
        ImGui::Dummy(ImVec2(0.0f, pad * 0.5f));

        // Icon on the left. Locked icons are dimmed so the state reads clearly.
        ImmediateTexture* icon = GetIcon(a);
        if (icon) {
          ImVec4 tint = is_unlocked ? style.unlocked_icon_tint : style.locked_icon_tint;
          ImGui::ImageWithBg(reinterpret_cast<ImTextureID>(icon),
                             ImVec2(style.icon_size * scale, style.icon_size * scale), ImVec2(0, 0),
                             ImVec2(1, 1), ImVec4(0, 0, 0, 0), tint);
        } else {
          ImGui::Dummy(ImVec2(style.icon_size * scale, style.icon_size * scale));
        }
        ImGui::SameLine();

        // Text block to the right of the icon.
        ImGui::BeginGroup();
        const char* marker = is_unlocked ? "[*]" : "[ ]";
        const ImVec4& title_color = is_unlocked ? style.unlocked_title : style.locked_title;
        ImGui::TextColored(title_color, "%s", marker);
        ImGui::SameLine();
        ImGui::TextColored(style.badge_gamerscore, "%dG", static_cast<int>(a.gamerscore));
        ImGui::SameLine();
        ImGui::TextColored(title_color, "%s", a.label.c_str());

        ImGui::PushStyleColor(ImGuiCol_Text, is_unlocked ? style.unlocked_desc : style.locked_desc);
        ImGui::TextWrapped("%s", desc.c_str());
        ImGui::PopStyleColor();
        ImGui::EndGroup();

        ImGui::Dummy(ImVec2(0.0f, pad * 0.5f));
        const float row_end_y = ImGui::GetCursorScreenPos().y;

        // Paint the band behind unlocked rows on the lower channel.
        if (is_unlocked) {
          draw_list->ChannelsSetCurrent(0);
          const float x0 = ImGui::GetWindowPos().x + 2.0f;
          const float x1 = x0 + ImGui::GetWindowSize().x - 4.0f;
          draw_list->AddRectFilled(ImVec2(x0, row_start_y), ImVec2(x1, row_end_y),
                                   ImGui::GetColorU32(style.row_unlocked_bg), style.row_rounding);
        }
        draw_list->ChannelsMerge();

        ImGui::Separator();
        ImGui::PopID();
      }
      ImGui::EndChild();
    };

    if (groups.custom.empty()) {
      draw_group(groups.original);
    } else if (ImGui::BeginTabBar("##ach-tabs")) {
      if (ImGui::BeginTabItem("Original")) {
        draw_group(groups.original);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Custom")) {
        draw_group(groups.custom);
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
  }
  ImGui::End();

  ImGui::PopStyleVar(2);
  if (!open && close_callback_) {
    close_callback_();
  }
}

}  // namespace rex::ui
