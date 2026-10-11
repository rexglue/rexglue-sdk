#include "ui/sdl3/overlay/achievement_icon_cache.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

#include <rex/runtime.h>
#include <rex/runtime/xdbf.h>
#include "ui/sdl3/image_decode.h"

namespace rex::ui {

AchievementIconCache::AchievementIconCache(gpu::IOverlayDrawer* overlay_drawer,
                                           rex::Runtime* runtime)
    : overlay_drawer_(overlay_drawer), runtime_(runtime) {}

gpu::OverlayTexture* AchievementIconCache::GetIcon(
    const rex::runtime::AchievementInfo& achievement) {
  const std::string cache_key =
      "path:" + achievement.icon_path + "|image:" + std::to_string(achievement.image_id);
  auto it = cache_.find(cache_key);
  if (it != cache_.end()) {
    return it->second.get();
  }

  texture_create_failed_ = false;
  std::unique_ptr<gpu::OverlayTexture> texture;
  if (!achievement.icon_path.empty()) {
    texture = LoadMetadataTexture(std::filesystem::path(achievement.icon_path));
  }
  if (!texture && achievement.image_id != 0) {
    texture = LoadMetadataTexture(std::filesystem::path("icons") /
                                  (std::to_string(achievement.image_id) + ".png"));
  }
  if (!texture && achievement.image_id != 0) {
    texture = LoadXDBFTexture(achievement.image_id);
  }

  if (!texture && texture_create_failed_) {
    return nullptr;
  }

  gpu::OverlayTexture* raw = texture.get();
  cache_.emplace(cache_key, std::move(texture));
  return raw;
}

std::unique_ptr<gpu::OverlayTexture> AchievementIconCache::CreateTextureFromBytes(const u8* data,
                                                                                  size_t size) {
  if (!overlay_drawer_ || !data || size == 0) {
    return nullptr;
  }

  int width = 0;
  int height = 0;
  std::vector<u8> rgba = DecodeImageRGBA(data, size, width, height);
  if (rgba.empty() || width <= 0 || height <= 0) {
    return nullptr;
  }

  auto texture = overlay_drawer_->CreateTexture(static_cast<u32>(width), static_cast<u32>(height),
                                                gpu::OverlayTextureFilter::kLinear, false,
                                                std::span<const u8>(rgba));
  if (!texture) {
    texture_create_failed_ = true;
    return nullptr;
  }
  return std::move(*texture);
}

std::unique_ptr<gpu::OverlayTexture> AchievementIconCache::LoadMetadataTexture(
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

        std::vector<u8> bytes(static_cast<size_t>(length));
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

std::unique_ptr<gpu::OverlayTexture> AchievementIconCache::LoadXDBFTexture(u32 image_id) {
  if (!runtime_ || !runtime_->is_setup()) {
    return nullptr;
  }

  const auto db = runtime_->content_manager().title_xdbf();
  if (!db.is_valid()) {
    return nullptr;
  }

  const auto block = db.GetEntry(rex::runtime::XDBFSection::kImage, static_cast<u64>(image_id));
  if (!block) {
    return nullptr;
  }
  return CreateTextureFromBytes(block.buffer, block.size);
}

}  // namespace rex::ui
