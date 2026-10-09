/**
 * @file        ui/sdl3/overlay/achievement_toast.h
 * @brief       Thread-safe achievement unlock toast - bottom-right corner,
 *              auto-dismisses after a few seconds.
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <rex/gpu/overlay.h>
#include <rex/macros.h>
#include "ui/sdl3/overlay/achievement_notification.h"
#include "ui/sdl3/overlay/achievement_icon_cache.h"

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::ui {

class AchievementToastDialog : public AchievementNotificationDialog {
 public:
  explicit AchievementToastDialog(const ModuleOverlayContext& context);
  ~AchievementToastDialog() override;

  /**
   * Queues an achievement toast. Safe to call from any thread, including guest threads.
   *
   * @param event Achievement event to show.
   */
  void Push(const rex::runtime::AchievementEvent& event) override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  static constexpr float kDisplaySeconds = 4.5f;

  struct PendingToast {
    rex::runtime::AchievementEvent event;
    std::chrono::steady_clock::time_point arrived;
    std::optional<std::chrono::steady_clock::time_point> visible_since = {};
  };

  std::mutex mutex_;
  std::deque<PendingToast> queue_;
  AchievementIconCache icon_cache_;
};

}  // namespace rex::ui
