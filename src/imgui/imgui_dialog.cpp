/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/ui/imgui_dialog.h>

#include <imgui.h>

namespace rex::ui {

ImGuiDialog::~ImGuiDialog() {
  if (host_) {
    host_->RemoveDialog(*this);
  }
}

bool ImGuiDialog::Closed() const {
  return closed_;
}

bool ImGuiDialog::Attached() const {
  return host_ != nullptr;
}

void ImGuiDialog::Attach(IDialogHost* host) {
  host_ = host;
}

void ImGuiDialog::Close() {
  close_pending_ = true;
}

void ImGuiDialog::Draw() {
  OnDraw(ImGui::GetIO());
  if (!close_pending_) {
    return;
  }
  close_pending_ = false;
  closed_ = true;
  OnClose();
  if (host_) {
    host_->RemoveDialog(*this);
  }
}

}  // namespace rex::ui
