/**
 * @file        ui/overlay/debug_overlay.cpp
 *
 * @brief       Debug overlay implementation. See debug_overlay.h for details.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/ui/keybinds.h>
#include <imgui.h>

namespace rex::ui {

DebugOverlayDialog::DebugOverlayDialog(ImGuiDrawer* imgui_drawer, FrameStatsProvider stats_provider)
    : ImGuiDialog(imgui_drawer), stats_provider_(std::move(stats_provider)) {
  RegisterBind("bind_debug_overlay", "F3", "Toggle debug overlay", [this] { ToggleVisible(); });
}

DebugOverlayDialog::~DebugOverlayDialog() {
  UnregisterBind("bind_debug_overlay");
}

void DebugOverlayDialog::OnDraw(ImGuiIO& io) {
  if (!visible_)
    return;

  ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(220, 60), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.5f);
  if (ImGui::Begin("Debug##overlay", nullptr, ImGuiWindowFlags_NoCollapse)) {
    ImGui::Text("Host:  %.1f FPS (%.2f ms)", io.Framerate, 1000.0f / io.Framerate);
    if (stats_provider_) {
      auto stats = stats_provider_();
      if (stats.frame_count > 0) {
        ImGui::Text("Guest: %.1f FPS (%.2f ms)", stats.fps, stats.frame_time_ms);
      }
    }
  }
  ImGui::End();
}

}  // namespace rex::ui
