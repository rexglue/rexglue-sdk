/**
 * @file        ui/sdl3/overlay/console_overlay.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include "ui/sdl3/console/model.h"
#include "ui/sdl3/console/view.h"
#include <rex/types.h>
#include <rex/platform.h>
#include <rex/ui/imgui_dialog.h>

namespace rex::ui {

class IUserInterface;
struct ModuleOverlayContext;

/** The console docked at the top left of the title window, resizable on its right and bottom. */
class ConsoleDialog : public ImGuiDialog {
 public:
  /**
   * Builds the dialog over the context's console model.
   *
   * @param context What the overlay may use.
   */
  explicit ConsoleDialog(const ModuleOverlayContext& context);

  /**
   * Shows or hides the OS window instead of the dialog while the console is undocked.
   *
   * @return True when undocked.
   */
  bool OnToggle() override;

 protected:
  /** Focuses the input. */
  void OnShow() override;

  /**
   * Draws the docked or minimized console.
   *
   * @param io The drawer's ImGui IO.
   */
  void OnDraw(ImGuiIO& io) override;

 private:
  IUserInterface& ui_;
  ConsoleModel& model_;
  ConsoleHost& host_;
  ConsoleView view_;
  bool minimized_ = false;
  bool first_draw_ = true;
};

}  // namespace rex::ui
