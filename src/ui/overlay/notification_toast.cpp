
/**
 * @file        ui/overlay/notification_toast.cpp
 * @brief       Notification toast. See notification_toast.h for details.
 */

#include <rex/ui/overlay/notification_toast.h>

#include <algorithm>

#include <imgui.h>
#include <imgui_internal.h>

#include "../notification_queue.h"

namespace rex::ui {

NotificationToastDialog::NotificationToastDialog(ImGuiDrawer* imgui_drawer)
    : ImGuiDialog(imgui_drawer) {}

NotificationToastDialog::~NotificationToastDialog() = default;

void NotificationToastDialog::OnDraw(ImGuiIO& io) {
  const auto toast = Notifications().Front(NotificationQueue::Clock::now());
  if (!toast) {
    return;
  }

  // Same fade envelope as the achievement toast.
  float alpha = 1.0f;
  if (toast->age_seconds < 0.3f) {
    alpha = toast->age_seconds / 0.3f;
  } else if (toast->age_seconds > toast->duration_seconds - 0.5f) {
    alpha = (toast->duration_seconds - toast->age_seconds) / 0.5f;
  }
  alpha = std::clamp(alpha, 0.0f, 1.0f);

  const Style& style = imgui_drawer()->style();
  const ToastStyle& s = style.toast;
  const float scale = imgui_drawer()->overlay_scale();
  const float pad = 16.0f * scale;
  const float width = 360.0f * scale;
  // Leave room for the achievement toast's height, including wrapped labels,
  // so both toasts can show at once.
  float achievement_slot = 80.0f * scale;
  if (const ImGuiWindow* achievement = ImGui::FindWindowByName("##ach_toast");
      achievement && achievement->WasActive) {
    achievement_slot = std::max(achievement_slot, achievement->Size.y);
  }
  const float inset = io.DisplaySize.y * std::clamp(s.bottom_inset, 0.0f, 0.5f);
  const float bottom = io.DisplaySize.y - inset - pad - achievement_slot - pad * 0.5f;
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - pad, bottom), ImGuiCond_Always,
                          ImVec2(1.0f, 1.0f));
  // Never taller than the space above the anchor, so the inset cannot push the top off screen.
  const float max_height = std::max(0.0f, std::min(io.DisplaySize.y * 0.5f, bottom - pad));
  ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, max_height));
  ImGui::SetNextWindowBgAlpha(style.chrome.draw_panel ? 0.0f : s.background_alpha * alpha);

  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus;
  auto faded = [alpha](ImVec4 c) { return ImVec4(c.x, c.y, c.z, c.w * alpha); };

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, s.rounding * scale);
  ImGui::PushStyleColor(ImGuiCol_Text, faded(s.text));
  if (ImGui::Begin("##rex_notification_toast", nullptr, flags)) {
    if (style.chrome.draw_panel && alpha > 0.0f) {
      const ImVec2 min = ImGui::GetWindowPos();
      const ImVec2 size = ImGui::GetWindowSize();
      const ImVec2 max(min.x + size.x, min.y + size.y);
      ImDrawList* draw_list = ImGui::GetWindowDrawList();
      draw_list->PushClipRect(min, max, false);
      style.chrome.draw_panel(draw_list, min, max);
      draw_list->PopClipRect();
    }
    ImGui::PushTextWrapPos(0.0f);
    if (!toast->title.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, faded(s.title));
      ImGui::TextUnformatted(toast->title.c_str());
      ImGui::PopStyleColor();
    }
    if (!toast->message.empty()) {
      ImGui::TextUnformatted(toast->message.c_str());
    }
    ImGui::PopTextWrapPos();
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
}

}  // namespace rex::ui
