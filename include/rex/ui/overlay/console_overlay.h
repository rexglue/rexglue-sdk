/**
 * @file        rex/ui/overlay/console_overlay.h
 * @brief       Console overlay docked in the title window
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/ui/console/model.h>
#include <rex/ui/console/view.h>
#include <rex/ui/imgui_dialog.h>

namespace rex::ui {

class OverlayContext;

/** The console docked at the top left of the title window, resizable on its right and bottom. */
class ConsoleDialog : public ImGuiDialog {
 public:
  /**
   * Builds the dialog over the context's console model.
   *
   * @param context What the overlay may use.
   */
  explicit ConsoleDialog(const OverlayContext& context);

  /**
   * Shows or hides the OS window instead of the dialog while the console is undocked.
   *
   * @return True when undocked.
   */
  bool OnToggle() override;

 protected:
  void OnShow() override;
  void OnDraw(ImGuiIO& io) override;

 private:
  ConsoleModel& model_;
  ConsoleHost& host_;
  ConsoleView view_;
  bool minimized_ = false;
  bool first_draw_ = true;
};

}  // namespace rex::ui
