#include "ui/sdl3/overlay/console_overlay.h"

#include <imgui.h>

#include "ui/sdl3/overlay_context.h"
#include <rex/ui/style.h>

namespace rex::ui {

ConsoleDialog::ConsoleDialog(const ModuleOverlayContext& context)
    : ui_(context.base.ui),
      model_(context.console),
      host_(context.console_host),
      view_(model_, host_) {}

bool ConsoleDialog::OnToggle() {
  if (model_.GetDock() != ConsoleDock::kUndocked) {
    return false;
  }
  host_.ToggleWindow();
  return true;
}

void ConsoleDialog::OnShow() {
  view_.FocusInput();
}

void ConsoleDialog::OnDraw(ImGuiIO& io) {
  if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f) {
    return;
  }
  const bool minimized = model_.GetDock() == ConsoleDock::kMinimized;
  if (first_draw_ || minimized != minimized_) {
    ImGui::SetNextWindowCollapsed(minimized, ImGuiCond_Always);
    minimized_ = minimized;
    first_draw_ = false;
  }
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
  if (!minimized) {
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * model_.GetWidthFraction(),
                                    io.DisplaySize.y * model_.GetHeightFraction()),
                             ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(io.DisplaySize.x * 0.2f, ImGui::GetFrameHeightWithSpacing() * 6.0f), io.DisplaySize);
  }
  ImGui::SetNextWindowBgAlpha(ui_.GetStyle().metrics.window_alpha.panel);
  const bool open = ImGui::Begin("Console##rex", nullptr,
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
  const bool collapsed = ImGui::IsWindowCollapsed();
  if (collapsed != minimized_) {
    model_.SetDock(collapsed ? ConsoleDock::kMinimized : ConsoleDock::kDocked);
    minimized_ = collapsed;
  }
  if (open && !collapsed) {
    const ImVec2 size = ImGui::GetWindowSize();
    model_.SetSize(size.x / io.DisplaySize.x, size.y / io.DisplaySize.y);
    view_.Draw(ui_.GetStyle().console);
  }
  ImGui::End();
}

}  // namespace rex::ui
