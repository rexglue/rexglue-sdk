#include <rex/ui/overlay/console_overlay.h>

#include <imgui.h>

#include <rex/ui/overlay.h>
#include <rex/ui/style.h>

namespace rex::ui {

ConsoleDialog::ConsoleDialog(const OverlayContext& context)
    : ImGuiDialog(&context.drawer()),
      model_(context.console()),
      host_(context.console_host()),
      view_(model_, host_) {}

bool ConsoleDialog::OnToggle() {
  if (model_.dock() != ConsoleDock::kUndocked) {
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
  const bool minimized = model_.dock() == ConsoleDock::kMinimized;
  if (first_draw_ || minimized != minimized_) {
    ImGui::SetNextWindowCollapsed(minimized, ImGuiCond_Always);
    minimized_ = minimized;
    first_draw_ = false;
  }
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
  if (!minimized) {
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * model_.width_fraction(),
                                    io.DisplaySize.y * model_.height_fraction()),
                             ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(io.DisplaySize.x * 0.2f, ImGui::GetFrameHeightWithSpacing() * 6.0f), io.DisplaySize);
  }
  ImGui::SetNextWindowBgAlpha(0.85f);
  const bool open = ImGui::Begin("Console##rex", nullptr,
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
  const bool collapsed = ImGui::IsWindowCollapsed();
  if (collapsed != minimized_) {
    model_.set_dock(collapsed ? ConsoleDock::kMinimized : ConsoleDock::kDocked);
    minimized_ = collapsed;
  }
  if (open && !collapsed) {
    const ImVec2 size = ImGui::GetWindowSize();
    model_.set_size(size.x / io.DisplaySize.x, size.y / io.DisplaySize.y);
    view_.Draw(imgui_drawer()->style().console);
  }
  ImGui::End();
}

}  // namespace rex::ui
