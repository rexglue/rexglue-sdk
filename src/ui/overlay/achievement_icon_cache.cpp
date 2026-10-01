/**
 * @file        ui/overlay/achievement_icon_cache.cpp
 * @brief       Shared achievement icon texture cache implementation.
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/achievement_icon_cache.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include <rex/runtime.h>
#include <rex/runtime/util/xdbf_utils.h>
#include <rex/ui/image_decode.h>
#include <rex/ui/immediate_drawer.h>

namespace rex::ui {

AchievementIconCache::AchievementIconCache(ImmediateDrawer* immediate_drawer, rex::Runtime* runtime)
    : immediate_drawer_(immediate_drawer), runtime_(runtime) {}

ImmediateTexture* AchievementIconCache::GetIcon(const rex::runtime::AchievementInfo& achievement) {
  const std::string cache_key =
      "path:" + achievement.icon_path + "|image:" + std::to_string(achievement.image_id);
  auto it = cache_.find(cache_key);
  if (it != cache_.end()) {
    return it->second.get();
  }

  std::unique_ptr<ImmediateTexture> texture;
  if (!achievement.icon_path.empty()) {
    texture = LoadMetadataTexture(std::filesystem::path(achievement.icon_path));
  }
  if (!texture && achievement.image_id != 0) {
    texture = LoadMetadataTexture(std::filesystem::path("icons") /
                                  (std::to_string(achievement.image_id) + ".png"));
  }
  if (!texture && achievement.image_id != 0) {
    texture = LoadXdbfTexture(achievement.image_id);
  }

  ImmediateTexture* raw = texture.get();
  cache_.emplace(cache_key, std::move(texture));
  return raw;
}

std::unique_ptr<ImmediateTexture> AchievementIconCache::CreateTextureFromBytes(const uint8_t* data,
                                                                               size_t size) {
  if (!immediate_drawer_ || !data || size == 0) {
    return nullptr;
  }

  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba = DecodeImageRGBA(data, size, width, height);
  if (rgba.empty() || width <= 0 || height <= 0) {
    return nullptr;
  }

  return immediate_drawer_->CreateTexture(static_cast<uint32_t>(width),
                                          static_cast<uint32_t>(height),
                                          ImmediateTextureFilter::kLinear, false, rgba.data());
}

std::unique_ptr<ImmediateTexture> AchievementIconCache::LoadMetadataTexture(
    const std::filesystem::path& relative_path) {
  if (!runtime_ || relative_path.empty()) {
    return nullptr;
  }

  auto path = runtime_->FindMetadataPath(relative_path);
  std::error_code ec;
  if (path && std::filesystem::exists(*path, ec)) {
    std::ifstream file(*path, std::ios::binary | std::ios::ate);
    if (file) {
      std::streamsize length = file.tellg();
      if (length > 0) {
        file.seekg(0);

        std::vector<uint8_t> bytes(static_cast<size_t>(length));
        if (file.read(reinterpret_cast<char*>(bytes.data()), length)) {
          return CreateTextureFromBytes(bytes.data(), bytes.size());
        }
      }
    }
  }

  if (auto embedded = runtime_->FindEmbeddedMetadata(relative_path)) {
    return CreateTextureFromBytes(embedded->bytes.data(), embedded->bytes.size());
  }
  return nullptr;
}

std::unique_ptr<ImmediateTexture> AchievementIconCache::LoadXdbfTexture(uint32_t image_id) {
  if (!runtime_ || !runtime_->is_setup()) {
    return nullptr;
  }

  const auto db = runtime_->content_manager().title_xdbf();
  if (!db.is_valid()) {
    return nullptr;
  }

  const auto block =
      db.GetEntry(rex::runtime::util::XdbfSection::kImage, static_cast<uint64_t>(image_id));
  if (!block) {
    return nullptr;
  }
  return CreateTextureFromBytes(block.buffer, block.size);
}

}  // namespace rex::ui
