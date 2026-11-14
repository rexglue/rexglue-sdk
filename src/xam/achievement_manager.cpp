#include "xam/achievement_manager.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <span>
#include <string>
#include <utility>

#include <rex/chrono.h>
#include <rex/logging/log_category.h>
#include <rex/platform/file.h>
#include <toml++/toml.hpp>

namespace rex::xam {

std::filesystem::path ResolveUnlockSavePath(const std::filesystem::path& user_root, u32 title_id) {
  return platform::ResolveEntryCase(platform::ResolveEntryCase(user_root, "achievements"),
                                    std::format("{:08X}.toml", title_id));
}

u64 AchievementManager::GetCurrentFileTime() {
  return WinSystemClock::ToFileTime(WinSystemClock::from_sys(std::chrono::system_clock::now()));
}

Result<void> AchievementManager::RegisterAchievement(Achievement info) {
  if (info.id == 0) {
    REXSYS_WARN("Achievement manager: refusing achievement with ID 0");
    return Err(ErrorCategory::kValidation, "achievement ID 0 is reserved");
  }

  std::lock_guard lock(mutex_);
  auto existing = std::find_if(achievements_.begin(), achievements_.end(),
                               [&](const Achievement& current) { return current.id == info.id; });
  if (existing != achievements_.end()) {
    *existing = std::move(info);
  } else {
    achievements_.push_back(std::move(info));
  }
  return Ok();
}

Result<void> AchievementManager::RegisterAchievements(std::span<const Achievement> achievements) {
  if (std::ranges::any_of(achievements, [](const Achievement& info) { return info.id == 0; })) {
    REXSYS_WARN("Achievement manager: refusing a batch with achievement ID 0");
    return Err(ErrorCategory::kValidation, "achievement ID 0 is reserved");
  }

  std::lock_guard lock(mutex_);
  for (const auto& info : achievements) {
    auto existing = std::ranges::find(achievements_, info.id,
                                      [](const Achievement& current) { return current.id; });
    if (existing != achievements_.end()) {
      *existing = info;
    } else {
      achievements_.push_back(info);
    }
  }
  return Ok();
}

void AchievementManager::ReplaceAchievements(std::vector<Achievement> achievements) {
  std::lock_guard lock(mutex_);
  achievements_.clear();
  for (auto& achievement : achievements) {
    if (achievement.id == 0) {
      REXSYS_WARN("Achievement manager: refusing achievement with ID 0");
      continue;
    }
    auto existing =
        std::find_if(achievements_.begin(), achievements_.end(),
                     [&](const Achievement& current) { return current.id == achievement.id; });
    if (existing != achievements_.end()) {
      *existing = std::move(achievement);
    } else {
      achievements_.push_back(std::move(achievement));
    }
  }
  REXSYS_DEBUG("Achievement manager: loaded {} entries", achievements_.size());
}

Result<void> AchievementManager::LoadMetadataFile(const std::filesystem::path& path) {
  try {
    auto table = toml::parse_file(path.string());
    const auto* entries = table["achievements"].as_array();
    if (!entries) {
      return Err(ErrorCategory::kFormat,
                 std::format("{} has no [[achievements]] entries", path.string()));
    }

    size_t loaded = 0;
    for (const auto& node : *entries) {
      const auto* entry = node.as_table();
      if (!entry) {
        continue;
      }

      Achievement info;
      info.id = static_cast<u32>((*entry)["id"].value_or<i64>(0));
      info.label = (*entry)["label"].value_or<std::string>("");
      info.description = (*entry)["description"].value_or<std::string>("");
      info.unachieved_description = (*entry)["unachieved_description"].value_or<std::string>("");
      info.icon_path = (*entry)["icon_path"].value_or<std::string>("");
      info.image_id = static_cast<u32>((*entry)["image_id"].value_or<i64>(0));
      info.gamerscore = static_cast<u32>((*entry)["gamerscore"].value_or<i64>(0));
      info.flags = static_cast<u32>((*entry)["flags"].value_or<i64>(0));
      if (RegisterAchievement(std::move(info))) {
        ++loaded;
      }
    }
    REXSYS_DEBUG("Loaded achievements.toml: {} entries from {}", loaded, path.string());
    return Ok();
  } catch (const toml::parse_error& error) {
    return Err(ErrorCategory::kFormat, error.what());
  }
}

std::vector<Achievement> AchievementManager::ListAchievements() const {
  std::lock_guard lock(mutex_);
  return achievements_;
}

rex::xam::AchievementUnlockResult AchievementManager::UnlockAchievement(
    u32 id, rex::xam::AchievementNotification notification) {
  rex::xam::AchievementEvent event;
  {
    std::lock_guard lock(mutex_);
    auto info = std::find_if(achievements_.begin(), achievements_.end(),
                             [&](const Achievement& current) { return current.id == id; });
    if (info == achievements_.end()) {
      REXSYS_WARN("Achievement manager: ignoring unknown achievement {:08X}", id);
      return rex::xam::AchievementUnlockResult::kUnknownAchievement;
    }

    u64 filetime = GetCurrentFileTime();
    auto [it, inserted] = unlocked_achievements_.emplace(id, filetime);
    if (!inserted) {
      return rex::xam::AchievementUnlockResult::kAlreadyUnlocked;
    }
    event = {*info};
  }

  REXSYS_INFO("Achievement unlocked: {:08X}", id);
  SaveUnlockState();
  if (notification == rex::xam::AchievementNotification::kShow) {
    DispatchCallbacks(notification_callbacks_, event);
  }
  return rex::xam::AchievementUnlockResult::kUnlocked;
}

bool AchievementManager::IsUnlocked(u32 id) const {
  std::lock_guard lock(mutex_);
  return unlocked_achievements_.contains(id);
}

std::optional<u64> AchievementManager::GetUnlockTime(u32 id) const {
  std::lock_guard lock(mutex_);
  auto it = unlocked_achievements_.find(id);
  if (it == unlocked_achievements_.end()) {
    return std::nullopt;
  }
  return it->second;
}

ScopeGuard AchievementManager::RegisterNotificationCallback(
    rex::xam::AchievementCallback callback) {
  if (!callback) {
    return ScopeGuard();
  }
  std::lock_guard lock(mutex_);
  const u64 handle = next_listener_handle_++;
  notification_callbacks_.emplace(handle, std::move(callback));
  return ScopeGuard([this, handle] {
    std::lock_guard guard(mutex_);
    notification_callbacks_.erase(handle);
  });
}

void AchievementManager::SetUnlockSavePath(std::filesystem::path path) {
  std::lock_guard lock(mutex_);
  unlock_save_path_ = std::move(path);
  unlocked_achievements_.clear();
}

void AchievementManager::LoadUnlockStateFrom(const std::filesystem::path& user_root, u32 title_id) {
  SetUnlockSavePath(ResolveUnlockSavePath(user_root, title_id));
  LoadUnlockState();
}

void AchievementManager::LoadUnlockState() {
  std::filesystem::path save_path;
  {
    std::lock_guard lock(mutex_);
    save_path = unlock_save_path_;
  }
  if (save_path.empty() || !std::filesystem::exists(save_path)) {
    return;
  }

  bool migrated = false;
  try {
    auto table = toml::parse_file(save_path.string());
    std::lock_guard lock(mutex_);
    if (const auto* unlocked = table["unlocked"].as_table()) {
      for (const auto& [key, value] : *unlocked) {
        u32 id = static_cast<u32>(std::stoul(std::string(key.str())));
        u64 filetime = 0;
        if (const auto* entry = value.as_table()) {
          filetime = static_cast<u64>((*entry)["filetime"].value_or<i64>(0));
        }
        unlocked_achievements_.emplace(id, filetime != 0 ? filetime : GetCurrentFileTime());
      }
    } else if (const auto* unlocked_ids = table["unlocked"].as_array()) {
      migrated = true;
      u64 now = GetCurrentFileTime();
      for (const auto& node : *unlocked_ids) {
        if (auto value = node.value<i64>()) {
          u32 id = static_cast<u32>(*value);
          if (id != 0) {
            unlocked_achievements_.emplace(id, now);
          }
        }
      }
    }
    if (!unlocked_achievements_.empty()) {
      REXSYS_DEBUG("Loaded {} persisted unlocks from {}", unlocked_achievements_.size(),
                   save_path.string());
    }
  } catch (const std::exception& error) {
    REXSYS_WARN("Failed to parse unlock save {}: {}", save_path.string(), error.what());
    return;
  }

  if (migrated) {
    REXSYS_DEBUG("Migrating legacy achievement save format: {}", save_path.string());
    SaveUnlockState();
  }
}

void AchievementManager::SaveUnlockState() const {
  std::lock_guard save_lock(save_mutex_);

  std::filesystem::path save_path;
  std::unordered_map<u32, u64> unlocked;
  {
    std::lock_guard lock(mutex_);
    save_path = unlock_save_path_;
    unlocked = unlocked_achievements_;
  }
  if (save_path.empty()) {
    return;
  }

  std::string content = "# Achievement unlock state managed by the ReXGlue runtime\n\n";
  for (const auto& [id, filetime] : unlocked) {
    content += std::format("[unlocked.{}]\nfiletime = {}\n\n", id, filetime);
  }

  auto written = rex::platform::WriteHostFile(
      save_path, std::span(reinterpret_cast<const u8*>(content.data()), content.size()));
  if (!written) {
    REXSYS_WARN("Achievement save: {}", written.error().message);
  }
}

void AchievementManager::DispatchCallbacks(
    const std::unordered_map<u64, rex::xam::AchievementCallback>& callbacks,
    const rex::xam::AchievementEvent& event) const {
  std::vector<rex::xam::AchievementCallback> snapshot;
  {
    std::lock_guard lock(mutex_);
    snapshot.reserve(callbacks.size());
    for (const auto& [handle, callback] : callbacks) {
      (void)handle;
      snapshot.push_back(callback);
    }
  }
  for (const auto& callback : snapshot) {
    callback(event);
  }
}

}  // namespace rex::xam
