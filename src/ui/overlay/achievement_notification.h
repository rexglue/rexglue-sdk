/**
 * @file        ui/overlay/achievement_notification.h
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/title.h>
#include <rex/ui/ui_system.h>

#include "ui/overlay/overlay_context.h"

namespace rex::ui {

class AchievementNotificationDialog : public DialogBase {
 public:
  ~AchievementNotificationDialog() override = default;

  virtual void Push(const Achievement& achievement) = 0;

 protected:
  explicit AchievementNotificationDialog(const ModuleOverlayContext& context)
      : ui_(context.base.ui) {}

  IUISystem& ui_;
};

}  // namespace rex::ui
