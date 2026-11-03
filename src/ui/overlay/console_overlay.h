/**
 * @file        ui/overlay/console_overlay.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include "ui/overlay/console/model.h"
#include "ui/overlay/console/view.h"
#include <rex/types.h>
#include <rex/ui/ui_system.h>
#include <rex/platform.h>

namespace rex::ui {

struct ModuleOverlayContext;

/** The console docked at the top left of the title window, resizable on its right and bottom. */
class ConsoleOverlayDialog : public DialogBase {
 public:
  explicit ConsoleOverlayDialog(const ModuleOverlayContext& context);

  /**
   * Shows or hides the OS window instead of the dialog while the console is undocked.
   *
   * @return True when undocked.
   */
  bool OnToggle() override;

  /** Takes all title input while the console shows. */
  bool WantsExclusiveInput() const override { return true; }

  /** Focuses the input. */
  void OnShow() override;

  bool Draw(ImGuiIO& io) override;

 private:
  IUISystem& ui_;
  ConsoleModel& model_;
  IConsoleHost& host_;
  ConsoleView view_;
  bool minimized_ = false;
  bool first_draw_ = true;
};

}  // namespace rex::ui
