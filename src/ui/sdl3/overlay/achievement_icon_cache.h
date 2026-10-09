/**
 * @file        ui/sdl3/overlay/achievement_icon_cache.h
 * @brief       Shared achievement icon texture cache for built-in overlays.
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

#include <rex/gpu/overlay.h>
#include <rex/macros.h>
#include <rex/runtime/achievement_info.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::ui {

class AchievementIconCache {
 public:
  AchievementIconCache(gpu::IOverlayDrawer* overlay_drawer, rex::Runtime* runtime);

  /**
   * Loads the achievement icon from the first source that resolves, in this order. The metadata
   * icon_path, the embedded metadata icon_path, metadata icons/<image_id>.png, embedded metadata
   * icons/<image_id>.png, then the title XDBF image <image_id> in the loaded XEX.
   *
   * @param achievement Achievement to load the icon for.
   * @return The icon texture, or null when none could be created.
   */
  gpu::OverlayTexture* GetIcon(const rex::runtime::AchievementInfo& achievement);

 private:
  std::unique_ptr<gpu::OverlayTexture> CreateTextureFromBytes(const u8* data, size_t size);
  std::unique_ptr<gpu::OverlayTexture> LoadMetadataTexture(
      const std::filesystem::path& relative_path);
  std::unique_ptr<gpu::OverlayTexture> LoadXdbfTexture(u32 image_id);

  gpu::IOverlayDrawer* overlay_drawer_ = nullptr;
  rex::Runtime* runtime_ = nullptr;
  bool texture_create_failed_ = false;
  std::unordered_map<std::string, std::unique_ptr<gpu::OverlayTexture>> cache_;
};

}  // namespace rex::ui
