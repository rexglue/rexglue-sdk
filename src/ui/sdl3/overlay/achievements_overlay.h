/**
 * @file        ui/sdl3/overlay/achievements_overlay.h
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <optional>

#include <rex/types.h>
#include <rex/platform.h>
#include <rex/runtime/achievement_manager.h>
#include <rex/ui/imgui_dialog.h>
#include "ui/sdl3/overlay/achievement_icon_cache.h"

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::ui {

struct ModuleOverlayContext;

class AchievementsOverlayDialog : public ImGuiDialog {
 public:
  explicit AchievementsOverlayDialog(const ModuleOverlayContext& context);
  ~AchievementsOverlayDialog() override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  /**
   * Lazily loads icon_path, or icons/<image_id>.png when icon_path is empty.
   *
   * @param achievement Achievement to load the icon for.
   * @return The icon texture, or null when none could be created.
   */
  gpu::OverlayTexture* GetIcon(const rex::runtime::AchievementInfo& achievement);

  const ModuleOverlayContext& context_;
  std::optional<AchievementIconCache> icon_cache_;
};

}  // namespace rex::ui
