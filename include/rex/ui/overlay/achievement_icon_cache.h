/**
 * @file        rex/ui/overlay/achievement_icon_cache.h
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
#include <rex/runtime/achievement_info.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::ui {

class AchievementIconCache {
 public:
  AchievementIconCache(gpu::IOverlayDrawer* overlay_drawer, rex::Runtime* runtime);

  // Loads the achievement icon in priority order:
  // 1. explicit metadata icon_path
  // 2. embedded metadata icon_path
  // 3. metadata icons/<image_id>.png
  // 4. embedded metadata icons/<image_id>.png
  // 5. title XDBF image <image_id> embedded in the loaded XEX
  gpu::OverlayTexture* GetIcon(const rex::runtime::AchievementInfo& achievement);

 private:
  std::unique_ptr<gpu::OverlayTexture> CreateTextureFromBytes(const uint8_t* data, size_t size);
  std::unique_ptr<gpu::OverlayTexture> LoadMetadataTexture(
      const std::filesystem::path& relative_path);
  std::unique_ptr<gpu::OverlayTexture> LoadXdbfTexture(uint32_t image_id);

  gpu::IOverlayDrawer* overlay_drawer_ = nullptr;
  rex::Runtime* runtime_ = nullptr;
  bool texture_create_failed_ = false;
  std::unordered_map<std::string, std::unique_ptr<gpu::OverlayTexture>> cache_;
};

}  // namespace rex::ui
