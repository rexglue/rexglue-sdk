/**
 * @file        ui/overlay/achievement_toast.cpp
 * @brief       Achievement toast implementation. See achievement_toast.h for details.
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/achievement_toast.h>
#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include <rex/ui/immediate_drawer.h>

namespace rex::ui {

AchievementToastDialog::AchievementToastDialog(ImGuiDrawer* drawer,
                                               ImmediateDrawer* immediate_drawer,
                                               rex::Runtime* runtime)
    : AchievementNotificationDialog(drawer), icon_cache_(immediate_drawer, runtime) {}

AchievementToastDialog::~AchievementToastDialog() {}

void AchievementToastDialog::Push(const rex::system::AchievementEvent& event) {
  std::lock_guard<std::mutex> lock(mutex_);
  queue_.push_back({event, std::chrono::steady_clock::now()});
}

void AchievementToastDialog::OnDraw(ImGuiIO& io) {
  auto now = std::chrono::steady_clock::now();

  PendingToast toast;
  float age = 0.0f;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!queue_.empty()) {
      if (!queue_.front().visible_since) {
        queue_.front().visible_since = now;
      }
      age = std::chrono::duration<float>(now - *queue_.front().visible_since).count();
      if (age >= kDisplaySeconds) {
        queue_.pop_front();
      } else {
        break;
      }
    }
    if (queue_.empty()) {
      return;
    }
    toast = queue_.front();
  }

  float alpha = 1.0f;
  if (age < 0.3f)
    alpha = age / 0.3f;
  else if (age > kDisplaySeconds - 0.5f)
    alpha = (kDisplaySeconds - age) / 0.5f;
  alpha = std::max(0.0f, std::min(1.0f, alpha));

  const ToastStyle& s = imgui_drawer()->style().toast;
  // Scale the frame with fonts and padding to avoid clipping at larger outputs.
  // Let the height fit the content.
  const float scale = imgui_drawer()->overlay_scale();
  const float pad = 16.0f * scale;
  const float w = 320.0f * scale;
  const float icon_size = s.icon_size * scale;
  const float inset = io.DisplaySize.y * std::clamp(s.bottom_inset, 0.0f, 0.5f);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - pad, io.DisplaySize.y - inset - pad),
                          ImGuiCond_Always, ImVec2(1.0f, 1.0f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0.0f), ImVec2(w, io.DisplaySize.y * 0.5f));
  ImGui::SetNextWindowBgAlpha(s.background_alpha * alpha);

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize;
  // Colors carry the fade envelope; the style holds them at full opacity.
  auto faded = [alpha](ImVec4 c) { return ImVec4(c.x, c.y, c.z, c.w * alpha); };

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, s.rounding * scale);
  ImGui::PushStyleColor(ImGuiCol_Text, faded(s.text));
  if (ImGui::Begin("##ach_toast", nullptr, flags)) {
    ImmediateTexture* icon = icon_cache_.GetIcon(toast.event.achievement);
    if (icon) {
      ImGui::Image(reinterpret_cast<ImTextureID>(icon), ImVec2(icon_size, icon_size));
      ImGui::SameLine();
    }

    ImGui::BeginGroup();
    // Long labels wrap inside the fixed width. The score follows the label when
    // both fit and otherwise takes its own line.
    ImGui::PushTextWrapPos(0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, faded(s.title));
    ImGui::TextUnformatted("Achievement Unlocked");
    ImGui::PopStyleColor();
    const std::string& label = toast.event.achievement.label;
    char score[32];
    std::snprintf(score, sizeof(score), " (%dG)",
                  static_cast<int>(toast.event.achievement.gamerscore));
    ImGui::TextUnformatted(label.c_str());
    if (ImGui::CalcTextSize(label.c_str()).x + ImGui::CalcTextSize(score).x <=
        ImGui::GetContentRegionAvail().x) {
      ImGui::SameLine(0.0f, 0.0f);
    }
    ImGui::TextDisabled("%s", score);
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
}

}  // namespace rex::ui
