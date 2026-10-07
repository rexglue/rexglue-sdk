/**
 * @file        ui/sdl3/overlay/achievements_overlay.cpp
 * @brief       Achievements overlay implementation. See achievements_overlay.h for details.
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include "ui/sdl3/overlay/achievements_overlay.h"

#include <imgui.h>

#include <rex/runtime.h>
#include <rex/gpu/overlay.h>
#include "ui/sdl3/overlay_context.h"
#include <rex/ui/color.h>

namespace rex::ui {

AchievementsOverlayDialog::AchievementsOverlayDialog(const ModuleOverlayContext& context)
    : context_(context) {}

AchievementsOverlayDialog::~AchievementsOverlayDialog() {}

gpu::OverlayTexture* AchievementsOverlayDialog::GetIcon(
    const rex::runtime::AchievementInfo& achievement) {
  return icon_cache_ ? icon_cache_->GetIcon(achievement) : nullptr;
}

void AchievementsOverlayDialog::OnDraw(ImGuiIO& io) {
  const Style& overlay_style = context_.base.ui.GetStyle();
  const AchievementsStyle& style = overlay_style.achievements;
  const Metrics& metrics = overlay_style.metrics;

  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 40.0f), ImGuiCond_FirstUseEver,
                          ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowSize(ImVec2(640.0f, 560.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(metrics.window_alpha.overlay);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                      ImVec2(style.window_padding.x, style.window_padding.y));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                      ImVec2(style.item_spacing.x, style.item_spacing.y));

  if (ImGui::Begin("Achievements##overlay", nullptr, ImGuiWindowFlags_NoCollapse)) {
    Runtime* runtime = context_.runtime;
    if (!runtime || !runtime->is_setup() || !context_.base.ui.OverlayDrawer()) {
      ImGui::TextUnformatted("No title is running.");
      ImGui::End();
      ImGui::PopStyleVar(2);
      return;
    }
    if (!icon_cache_) {
      icon_cache_.emplace(context_.base.ui.OverlayDrawer(), runtime);
    }
    auto& manager = runtime->achievement_manager();
    const auto achievements = manager.ListAchievements();

    int unlocked_count = 0;
    int total_gs = 0;
    int earned_gs = 0;
    for (const auto& a : achievements) {
      total_gs += static_cast<int>(a.gamerscore);
      if (manager.IsUnlocked(a.id)) {
        ++unlocked_count;
        earned_gs += static_cast<int>(a.gamerscore);
      }
    }
    const int total_count = static_cast<int>(achievements.size());

    // ---- Header: summary line + progress bar -------------------------------
    ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(style.header_text));
    ImGui::Text("%d / %d unlocked", unlocked_count, total_count);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextColored(ToImVec4(style.badge_gamerscore), "%dG / %dG", earned_gs, total_gs);

    float frac = total_count > 0 ? static_cast<float>(unlocked_count) / total_count : 0.0f;
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ToImVec4(style.progress_bar));
    ImGui::ProgressBar(frac, ImVec2(-1.0f, 6.0f), "");
    ImGui::PopStyleColor();

    ImGui::TextDisabled("Unlock progress is saved automatically");
    ImGui::Separator();
    ImGui::Spacing();

    // ---- List --------------------------------------------------------------
    ImGui::BeginChild("##achlist", ImVec2(0.0f, 0.0f), false);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    for (const auto& a : achievements) {
      const bool is_unlocked = manager.IsUnlocked(a.id);
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
      gpu::OverlayTexture* icon = GetIcon(a);
      if (icon) {
        const Color tint = is_unlocked ? style.unlocked_icon_tint : style.locked_icon_tint;
        ImGui::ImageWithBg(reinterpret_cast<ImTextureID>(icon),
                           ImVec2(metrics.icon_size, metrics.icon_size), ImVec2(0, 0), ImVec2(1, 1),
                           ToImVec4(colors::kTransparent), ToImVec4(tint));
      } else {
        ImGui::Dummy(ImVec2(metrics.icon_size, metrics.icon_size));
      }
      ImGui::SameLine();

      // Text block to the right of the icon.
      ImGui::BeginGroup();
      const char* marker = is_unlocked ? "[*]" : "[ ]";
      const ImVec4 title_color = ToImVec4(is_unlocked ? style.unlocked_title : style.locked_title);
      ImGui::TextColored(title_color, "%s", marker);
      ImGui::SameLine();
      ImGui::TextColored(ToImVec4(style.badge_gamerscore), "%dG", static_cast<i32>(a.gamerscore));
      ImGui::SameLine();
      ImGui::TextColored(title_color, "%s", a.label.c_str());

      ImGui::PushStyleColor(ImGuiCol_Text,
                            ToImVec4(is_unlocked ? style.unlocked_desc : style.locked_desc));
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
                                 ImGui::GetColorU32(ToImVec4(style.row_unlocked_bg)),
                                 metrics.row_rounding);
      }
      draw_list->ChannelsMerge();

      ImGui::Separator();
      ImGui::PopID();
    }
    ImGui::EndChild();
  }
  ImGui::End();

  ImGui::PopStyleVar(2);
}

}  // namespace rex::ui
