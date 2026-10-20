/**
 * @file        rex/ui/overlay/achievement_toast.h
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
#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
#include <rex/ui/overlay/achievement_notification.h>
#include <rex/ui/overlay/achievement_icon_cache.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::ui {

class ImmediateDrawer;

class AchievementToastDialog : public AchievementNotificationDialog {
 public:
  AchievementToastDialog(ImGuiDrawer* drawer, ImmediateDrawer* immediate_drawer,
                         rex::Runtime* runtime);
  ~AchievementToastDialog() override;

  // Thread-safe: safe to call from any thread, including guest threads.
  void Push(const rex::system::AchievementEvent& event) override;

  // The window repaints over and over only while a toast is on screen (to
  // fade it in and out). The dialog exists for the whole session, so with the
  // default (always) the window repainted and presented at the display's rate
  // on top of the game's own frames all the time, for nothing.
  bool WantsContinuousRepaint() const override {
    return showing_.load(std::memory_order_relaxed);
  }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  static constexpr float kDisplaySeconds = 4.5f;

  struct PendingToast {
    rex::system::AchievementEvent event;
    std::chrono::steady_clock::time_point arrived;
    std::optional<std::chrono::steady_clock::time_point> visible_since;
  };

  std::mutex mutex_;
  std::deque<PendingToast> queue_;
  std::atomic<bool> showing_{false};  // queue_ not empty; set under mutex_
  AchievementIconCache icon_cache_;
};

}  // namespace rex::ui
