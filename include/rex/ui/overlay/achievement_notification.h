/**
 * @file        rex/ui/overlay/achievement_notification.h
 * @brief       Replaceable achievement notification dialog interface.
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/runtime/achievement_manager.h>
#include <rex/ui/imgui_dialog.h>

namespace rex::ui {

class AchievementNotificationDialog : public ImGuiDialog {
 public:
  ~AchievementNotificationDialog() override = default;

  // Thread-safe implementations may receive events from guest threads.
  virtual void Push(const rex::runtime::AchievementEvent& event) = 0;

 protected:
  explicit AchievementNotificationDialog(ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
};

}  // namespace rex::ui
