#include "runtime/achievement_manager.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <span>
#include <string>
#include <utility>

#include <rex/logging/log.h>
#include <rex/platform/file.h>
#include <toml++/toml.hpp>

namespace rex::runtime {

u64 AchievementManager::GetCurrentFileTime() {
  using namespace std::chrono;
  auto now = system_clock::now().time_since_epoch();
  auto intervals = duration_cast<duration<i64, std::ratio<1, 10'000'000>>>(now);
  constexpr u64 kEpochDelta = 116'444'736'000'000'000ULL;
  return static_cast<u64>(intervals.count()) + kEpochDelta;
}

Result<void> AchievementManager::RegisterAchievement(AchievementInfo info) {
  if (info.id == 0) {
    REXLOG_WARN(system, "Achievement store: refusing achievement with ID 0");
    return Err(ErrorCategory::kValidation, "achievement ID 0 is reserved");
  }

  std::lock_guard lock(mutex_);
  auto existing =
      std::find_if(achievements_.begin(), achievements_.end(),
                   [&](const AchievementInfo& current) { return current.id == info.id; });
  if (existing != achievements_.end()) {
    *existing = std::move(info);
  } else {
    achievements_.push_back(std::move(info));
  }
  return Ok();
}

Result<void> AchievementManager::RegisterAchievements(
    std::span<const AchievementInfo> achievements) {
  if (std::ranges::any_of(achievements, [](const AchievementInfo& info) { return info.id == 0; })) {
    REXLOG_WARN(system, "Achievement store: refusing a batch with achievement ID 0");
    return Err(ErrorCategory::kValidation, "achievement ID 0 is reserved");
  }

  std::lock_guard lock(mutex_);
  for (const auto& info : achievements) {
    auto existing = std::ranges::find(achievements_, info.id,
                                      [](const AchievementInfo& current) { return current.id; });
    if (existing != achievements_.end()) {
      *existing = info;
    } else {
      achievements_.push_back(info);
    }
  }
  return Ok();
}

void AchievementManager::ReplaceAchievements(std::vector<AchievementInfo> achievements) {
  std::lock_guard lock(mutex_);
  achievements_.clear();
  for (auto& achievement : achievements) {
    if (achievement.id == 0) {
      REXLOG_WARN(system, "Achievement store: refusing achievement with ID 0");
      continue;
    }
    auto existing =
        std::find_if(achievements_.begin(), achievements_.end(),
                     [&](const AchievementInfo& current) { return current.id == achievement.id; });
    if (existing != achievements_.end()) {
      *existing = std::move(achievement);
    } else {
      achievements_.push_back(std::move(achievement));
    }
  }
  REXLOG_DEBUG(system, "Achievement store: loaded {} entries", achievements_.size());
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

      AchievementInfo info;
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
    REXLOG_DEBUG(system, "Loaded achievements.toml: {} entries from {}", loaded, path.string());
    return Ok();
  } catch (const toml::parse_error& error) {
    return Err(ErrorCategory::kFormat, error.what());
  }
}

std::vector<AchievementInfo> AchievementManager::ListAchievements() const {
  std::lock_guard lock(mutex_);
  return achievements_;
}

AchievementUnlockResult AchievementManager::UnlockAchievement(
    u32 id, AchievementNotification notification) {
  AchievementEvent event;
  {
    std::lock_guard lock(mutex_);
    auto info = std::find_if(achievements_.begin(), achievements_.end(),
                             [&](const AchievementInfo& current) { return current.id == id; });
    if (info == achievements_.end()) {
      REXLOG_WARN(system, "Achievement store: ignoring unknown achievement {:08X}", id);
      return AchievementUnlockResult::kUnknownAchievement;
    }

    u64 filetime = GetCurrentFileTime();
    auto [it, inserted] = unlocked_achievements_.emplace(id, filetime);
    if (!inserted) {
      return AchievementUnlockResult::kAlreadyUnlocked;
    }
    event = {*info};
  }

  REXLOG_INFO(system, "Achievement unlocked: {:08X}", id);
  SaveUnlockState();
  if (notification == AchievementNotification::kShow) {
    DispatchCallbacks(notification_callbacks_, event);
  }
  return AchievementUnlockResult::kUnlocked;
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

AchievementListenerHandle AchievementManager::RegisterNotificationCallback(
    AchievementCallback callback) {
  if (!callback) {
    return 0;
  }
  std::lock_guard lock(mutex_);
  AchievementListenerHandle handle = next_listener_handle_++;
  notification_callbacks_.emplace(handle, std::move(callback));
  return handle;
}

void AchievementManager::UnregisterCallback(AchievementListenerHandle handle) {
  std::lock_guard lock(mutex_);
  notification_callbacks_.erase(handle);
}

void AchievementManager::SetUnlockSavePath(std::filesystem::path path) {
  std::lock_guard lock(mutex_);
  unlock_save_path_ = std::move(path);
  unlocked_achievements_.clear();
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
      REXLOG_DEBUG(system, "Loaded {} persisted unlocks from {}", unlocked_achievements_.size(),
                   save_path.string());
    }
  } catch (const std::exception& error) {
    REXLOG_WARN(system, "Failed to parse unlock save {}: {}", save_path.string(), error.what());
    return;
  }

  if (migrated) {
    REXLOG_DEBUG(system, "Migrating legacy achievement save format: {}", save_path.string());
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

  std::string content = "# Achievement unlock state - managed by ReXGlue runtime\n\n";
  for (const auto& [id, filetime] : unlocked) {
    content += std::format("[unlocked.{}]\nfiletime = {}\n\n", id, filetime);
  }

  auto written = rex::platform::WriteHostFile(
      save_path, std::span(reinterpret_cast<const u8*>(content.data()), content.size()));
  if (!written) {
    REXLOG_WARN(system, "Achievement save: {}", written.error().message);
  }
}

void AchievementManager::DispatchCallbacks(
    const std::unordered_map<AchievementListenerHandle, AchievementCallback>& callbacks,
    const AchievementEvent& event) const {
  std::vector<AchievementCallback> snapshot;
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

}  // namespace rex::runtime
