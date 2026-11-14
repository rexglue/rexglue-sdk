#include "ui/overlay/achievement_overlay.h"

#include <imgui.h>

#include "ui/overlay/overlay_context.h"
#include <rex/ui/style.h>

namespace rex::ui {

AchievementsOverlayDialog::AchievementsOverlayDialog(const ModuleOverlayContext& context)
    : context_(context) {}

AchievementsOverlayDialog::~AchievementsOverlayDialog() {}

ImTextureData* AchievementsOverlayDialog::GetIcon(const Achievement& achievement) {
  return icon_cache_ ? icon_cache_->GetIcon(achievement) : nullptr;
}

bool AchievementsOverlayDialog::Draw(ImGuiIO& io) {
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
    IAchievementRegistry* source = context_.achievements;
    if (!source) {
      icon_cache_.reset();
      ImGui::TextUnformatted("No title is running.");
      ImGui::End();
      ImGui::PopStyleVar(2);
      return true;
    }
    if (!icon_cache_) {
      icon_cache_.emplace(source);
    }
    const auto achievements = source->GetAchievements();

    int unlocked_count = 0;
    int total_gs = 0;
    int earned_gs = 0;
    for (const auto& a : achievements) {
      total_gs += static_cast<int>(a.gamerscore);
      if (a.unlocked) {
        ++unlocked_count;
        earned_gs += static_cast<int>(a.gamerscore);
      }
    }
    const int total_count = static_cast<int>(achievements.size());

    ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(style.header_text));
    ImGui::Text("%d / %d unlocked", unlocked_count, total_count);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextColored(ToImVec4(style.badge_gamerscore), "%dG / %dG", earned_gs, total_gs);

    f32 frac =
        total_count > 0 ? static_cast<f32>(unlocked_count) / static_cast<f32>(total_count) : 0.0f;
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ToImVec4(style.progress_bar));
    ImGui::ProgressBar(frac, ImVec2(-1.0f, 6.0f), "");
    ImGui::PopStyleColor();

    ImGui::TextDisabled("Unlock progress is saved automatically");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::BeginChild("##achlist", ImVec2(0.0f, 0.0f), false);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    for (const auto& a : achievements) {
      const bool is_unlocked = a.unlocked;
      const std::string& desc = a.GetDescription();

      ImGui::PushID(static_cast<int>(a.id));

      // Channel 0 holds the row band behind the text, painted after the row is measured.
      draw_list->ChannelsSplit(2);
      draw_list->ChannelsSetCurrent(1);

      const f32 row_start_y = ImGui::GetCursorScreenPos().y;
      const f32 pad = 4.0f;
      ImGui::Dummy(ImVec2(0.0f, pad * 0.5f));

      ImTextureData* icon = GetIcon(a);
      if (icon) {
        const Color tint = is_unlocked ? style.unlocked_icon_tint : style.locked_icon_tint;
        ImGui::ImageWithBg(ImTextureRef(icon), ImVec2(metrics.icon_size, metrics.icon_size),
                           ImVec2(0, 0), ImVec2(1, 1), ToImVec4(colors::kTransparent),
                           ToImVec4(tint));
      } else {
        ImGui::Dummy(ImVec2(metrics.icon_size, metrics.icon_size));
      }
      ImGui::SameLine();

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
      const f32 row_end_y = ImGui::GetCursorScreenPos().y;

      if (is_unlocked) {
        draw_list->ChannelsSetCurrent(0);
        const f32 x0 = ImGui::GetWindowPos().x + 2.0f;
        const f32 x1 = x0 + ImGui::GetWindowSize().x - 4.0f;
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
  return true;
}

}  // namespace rex::ui
