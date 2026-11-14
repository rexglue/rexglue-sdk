/**
 * @file        ui/overlay/achievement_overlay.h
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
#include <rex/title.h>
#include <rex/ui/ui_system.h>
#include "ui/overlay/achievement_icon_cache.h"

namespace rex::ui {

struct ModuleOverlayContext;

class AchievementsOverlayDialog : public DialogBase {
 public:
  explicit AchievementsOverlayDialog(const ModuleOverlayContext& context);
  ~AchievementsOverlayDialog() override;

  bool Draw(ImGuiIO& io) override;

 private:
  ImTextureData* GetIcon(const Achievement& achievement);

  const ModuleOverlayContext& context_;
  std::optional<AchievementIconCache> icon_cache_;
};

}  // namespace rex::ui
