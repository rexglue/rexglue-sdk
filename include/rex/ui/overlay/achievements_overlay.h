/**
 * @file        rex/ui/overlay/achievements_overlay.h
 * @brief       Default ImGui achievements overlay dialog.
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <optional>

#include <rex/runtime/achievement_manager.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/overlay/achievement_icon_cache.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::ui {

class ImmediateDrawer;
class ImmediateTexture;
class OverlayContext;

class AchievementsOverlayDialog : public ImGuiDialog {
 public:
  explicit AchievementsOverlayDialog(const OverlayContext& context);
  ~AchievementsOverlayDialog() override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  // Lazily loads icon_path, or icons/<image_id>.png when icon_path is empty.
  ImmediateTexture* GetIcon(const rex::runtime::AchievementInfo& achievement);

  const OverlayContext& context_;
  std::optional<AchievementIconCache> icon_cache_;
};

}  // namespace rex::ui
