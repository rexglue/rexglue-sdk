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

#ifndef REX_UI_IMGUI_DIALOG_H_
#define REX_UI_IMGUI_DIALOG_H_

#include <memory>
#include <vector>

#include <rex/thread.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/window_listener.h>

struct ImGuiIO;

namespace rex {
namespace ui {

class ImGuiDialog {
 public:
  ~ImGuiDialog();

  // Shows a simple message box containing a text message.
  // Callers can want for the dialog to close with Wait().
  // Dialogs retain themselves and will delete themselves when closed.
  static ImGuiDialog* ShowMessageBox(ImGuiDrawer* imgui_drawer, std::string title,
                                     std::string body);

  // A fence to signal when the dialog is closed.
  void Then(rex::thread::Fence* fence);

  void Draw();

 protected:
  ImGuiDialog(ImGuiDrawer* imgui_drawer);

  ImGuiDrawer* imgui_drawer() const { return imgui_drawer_; }
  ImGuiIO& GetIO();

  // Closes the dialog and returns to any waiters.
  void Close();

  virtual void OnShow() {}
  virtual void OnClose() {}
  virtual void OnDraw(ImGuiIO& io) { (void)io; }

 private:
  ImGuiDrawer* imgui_drawer_ = nullptr;
  bool has_close_pending_ = false;
  std::vector<rex::thread::Fence*> waiting_fences_;
};

}  // namespace ui
}  // namespace rex

#endif  // REX_UI_IMGUI_DIALOG_H_
