/**
 * @file        ui/overlay/achievement_icon_cache.h
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

#include <rex/platform.h>
#include <rex/types.h>
#include <imgui.h>

#include <rex/title.h>

namespace rex::ui {

class AchievementIconCache {
 public:
  explicit AchievementIconCache(IAchievementRegistry* achievements);
  ~AchievementIconCache();

  /**
   * Returns the icon texture for an achievement, loading it on first use, or null when none could
   * be created.
   */
  ImTextureData* GetIcon(const Achievement& achievement);

 private:
  std::unique_ptr<ImTextureData> CreateTextureFromBytes(const u8* data, size_t size);
  std::unique_ptr<ImTextureData> LoadTexture(u32 image_id);

  IAchievementRegistry* achievements_ = nullptr;
  std::unordered_map<std::string, std::unique_ptr<ImTextureData>> cache_;
};

}  // namespace rex::ui
