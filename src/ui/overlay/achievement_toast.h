/**
 * @file        ui/overlay/achievement_toast.h
 * @brief       Unlock toast in the bottom right corner that dismisses itself.
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
#include <rex/platform.h>
#include <rex/types.h>
#include "ui/overlay/achievement_notification.h"
#include "ui/overlay/achievement_icon_cache.h"

namespace rex::ui {

class AchievementToastDialog : public AchievementNotificationDialog {
 public:
  explicit AchievementToastDialog(const ModuleOverlayContext& context);
  ~AchievementToastDialog() override;

  /** Queues a toast. Safe to call from any thread. */
  void Push(const Achievement& achievement) override;

  bool NeedsContinuousPaint() const override;

  bool Draw(ImGuiIO& io) override;

 private:
  static constexpr f32 kDisplaySeconds = 4.5f;

  struct PendingToast {
    Achievement achievement;
    std::chrono::steady_clock::time_point arrived;
    // NOLINTNEXTLINE(readability-redundant-member-init) designated initializers omit this field
    std::optional<std::chrono::steady_clock::time_point> visible_since = {};
  };

  mutable std::mutex mutex_;
  std::deque<PendingToast> queue_;
  AchievementIconCache icon_cache_;
};

}  // namespace rex::ui
