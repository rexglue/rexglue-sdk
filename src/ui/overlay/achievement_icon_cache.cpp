#include "ui/overlay/achievement_icon_cache.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

#include <imgui_internal.h>

#include "ui/overlay/image_decode.h"

namespace rex::ui {

AchievementIconCache::AchievementIconCache(IAchievementRegistry* achievements)
    : achievements_(achievements) {}

AchievementIconCache::~AchievementIconCache() {
  if (ImGui::GetCurrentContext()) {
    for (auto& entry : cache_) {
      if (entry.second) {
        ImGui::UnregisterUserTexture(entry.second.get());
      }
    }
  }
}

ImTextureData* AchievementIconCache::GetIcon(const Achievement& achievement) {
  if (achievement.image_id == 0) {
    return nullptr;
  }
  const std::string cache_key = std::to_string(achievement.image_id);
  auto it = cache_.find(cache_key);
  if (it != cache_.end()) {
    return it->second.get();
  }
  std::unique_ptr<ImTextureData> texture = LoadTexture(achievement.image_id);
  ImTextureData* raw = texture.get();
  cache_.emplace(cache_key, std::move(texture));
  return raw;
}

std::unique_ptr<ImTextureData> AchievementIconCache::CreateTextureFromBytes(const u8* data,
                                                                            size_t size) {
  if (!data || size == 0) {
    return nullptr;
  }
  int width = 0;
  int height = 0;
  std::vector<u8> rgba = DecodeImageRGBA(data, size, width, height);
  if (rgba.empty() || width <= 0 || height <= 0) {
    return nullptr;
  }
  auto texture = std::make_unique<ImTextureData>();
  texture->Create(ImTextureFormat_RGBA32, width, height);
  std::memcpy(texture->GetPixels(), rgba.data(), rgba.size());
  texture->UseColors = true;
  ImGui::RegisterUserTexture(texture.get());
  return texture;
}

std::unique_ptr<ImTextureData> AchievementIconCache::LoadTexture(u32 image_id) {
  if (!achievements_) {
    return nullptr;
  }
  auto bytes = achievements_->GetAchievementIcon(image_id);
  if (!bytes || bytes->empty()) {
    return nullptr;
  }
  return CreateTextureFromBytes(reinterpret_cast<const u8*>(bytes->data()), bytes->size());
}

}  // namespace rex::ui
