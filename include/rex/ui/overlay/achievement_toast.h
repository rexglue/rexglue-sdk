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

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
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
  AchievementIconCache icon_cache_;

  // The dialog is only registered with the ImGuiDrawer while it has a toast to
  // show. A registered dialog makes the presenter repaint continuously on the
  // UI thread at the display refresh rate (instead of presenting each guest
  // frame from the guest output thread), so an always-registered empty toast
  // doubled presentation work and made FPS overlays report the refresh rate.
  bool registered_ = false;                // UI thread only.
  std::atomic<bool> add_queued_{false};    // A deferred AddDialog is pending.
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};

}  // namespace rex::ui
