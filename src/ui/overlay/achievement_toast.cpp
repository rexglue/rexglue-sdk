#include "ui/overlay/achievement_toast.h"
#include <imgui.h>

#include <algorithm>

#include <rex/ui/style.h>

namespace rex::ui {

AchievementToastDialog::AchievementToastDialog(const ModuleOverlayContext& context)
    : AchievementNotificationDialog(context), icon_cache_(context.achievements) {}

AchievementToastDialog::~AchievementToastDialog() {}

void AchievementToastDialog::Push(const Achievement& achievement) {
  std::lock_guard<std::mutex> lock(mutex_);
  queue_.push_back({achievement, std::chrono::steady_clock::now()});
}

bool AchievementToastDialog::NeedsContinuousPaint() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return !queue_.empty();
}

bool AchievementToastDialog::Draw(ImGuiIO& io) {
  auto now = std::chrono::steady_clock::now();

  PendingToast toast;
  f32 age = 0.0f;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!queue_.empty()) {
      auto& visible_since = queue_.front().visible_since;
      if (!visible_since) {
        visible_since = now;
      }
      age = std::chrono::duration<f32>(now - *visible_since).count();
      if (age >= kDisplaySeconds) {
        queue_.pop_front();
      } else {
        break;
      }
    }
    if (queue_.empty()) {
      return true;
    }
    toast = queue_.front();
  }

  f32 alpha = 1.0f;
  if (age < 0.3f)
    alpha = age / 0.3f;
  else if (age > kDisplaySeconds - 0.5f)
    alpha = (kDisplaySeconds - age) / 0.5f;
  alpha = std::max(0.0f, std::min(1.0f, alpha));

  const Style& style = ui_.GetStyle();
  const Metrics& metrics = style.metrics;
  const ToastStyle& s = style.toast;
  const f32 pad = metrics.toast_padding;
  const f32 w = metrics.toast_width;
  ImGui::SetNextWindowPos(
      ImVec2(io.DisplaySize.x - w - pad, io.DisplaySize.y - metrics.toast_stack_height - pad),
      ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(w, metrics.toast_height), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(metrics.window_alpha.toast * alpha);

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoInputs;
  auto faded = [alpha](Color c) { return ToImVec4(c.WithAlpha(c.a * alpha)); };

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, metrics.toast_rounding);
  ImGui::PushStyleColor(ImGuiCol_Text, faded(s.text));
  if (ImGui::Begin("##ach_toast", nullptr, flags)) {
    ImTextureData* icon = icon_cache_.GetIcon(toast.achievement);
    if (icon) {
      ImGui::Image(ImTextureRef(icon), ImVec2(metrics.icon_size, metrics.icon_size));
      ImGui::SameLine();
    }

    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_Text, faded(s.title));
    ImGui::TextUnformatted("Achievement Unlocked");
    ImGui::PopStyleColor();
    ImGui::TextUnformatted(toast.achievement.label.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled(" (%dG)", static_cast<int>(toast.achievement.gamerscore));
    ImGui::EndGroup();
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
  return true;
}

}  // namespace rex::ui
