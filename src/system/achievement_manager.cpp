/**
 * @file        system/achievement_manager.cpp
 * @brief       AchievementManager implementation.
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/system/achievement_manager.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <string>
#include <utility>

#include <fmt/format.h>
#include <rex/logging.h>
#include <toml++/toml.hpp>

namespace rex::system {

uint64_t AchievementManager::CurrentFileTime() {
  using namespace std::chrono;
  auto now = system_clock::now().time_since_epoch();
  auto intervals = duration_cast<duration<int64_t, std::ratio<1, 10'000'000>>>(now);
  constexpr uint64_t kEpochDelta = 116'444'736'000'000'000ULL;
  return static_cast<uint64_t>(intervals.count()) + kEpochDelta;
}

bool AchievementManager::RegisterAchievement(AchievementInfo info) {
  if (info.id == 0) {
    REXSYS_WARN("Achievement store: refusing achievement with ID 0");
    return false;
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
  return true;
}

void AchievementManager::RegisterAchievements(std::span<const AchievementInfo> achievements) {
  for (const auto& achievement : achievements) {
    RegisterAchievement(achievement);
  }
}

void AchievementManager::ReplaceAchievements(std::vector<AchievementInfo> achievements) {
  std::lock_guard lock(mutex_);
  achievements_.clear();
  for (auto& achievement : achievements) {
    if (achievement.id == 0) {
      REXSYS_WARN("Achievement store: refusing achievement with ID 0");
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
  REXSYS_DEBUG("Achievement store: loaded {} entries", achievements_.size());
}

bool AchievementManager::LoadMetadataFile(const std::filesystem::path& path) {
  try {
    auto table = toml::parse_file(path.string());
    const auto* entries = table["achievements"].as_array();
    if (!entries) {
      REXSYS_WARN("Achievement metadata has no [[achievements]] entries: {}", path.string());
      return false;
    }

    size_t loaded = 0;
    for (const auto& node : *entries) {
      const auto* entry = node.as_table();
      if (!entry) {
        continue;
      }

      AchievementInfo info;
      info.id = static_cast<uint32_t>((*entry)["id"].value_or<int64_t>(0));
      info.label = (*entry)["label"].value_or<std::string>("");
      info.description = (*entry)["description"].value_or<std::string>("");
      info.unachieved_description = (*entry)["unachieved_description"].value_or<std::string>("");
      info.icon_path = (*entry)["icon_path"].value_or<std::string>("");
      info.image_id = static_cast<uint32_t>((*entry)["image_id"].value_or<int64_t>(0));
      info.gamerscore = static_cast<uint32_t>((*entry)["gamerscore"].value_or<int64_t>(0));
      info.flags = static_cast<uint32_t>((*entry)["flags"].value_or<int64_t>(0));
      info.custom = (*entry)["custom"].value_or(false);
      if (RegisterAchievement(std::move(info))) {
        ++loaded;
      }
    }
    REXSYS_DEBUG("Loaded achievements.toml: {} entries from {}", loaded, path.string());
    return true;
  } catch (const toml::parse_error& error) {
    REXSYS_WARN("Failed to parse {}: {}", path.string(), error.what());
    return false;
  }
}

std::vector<AchievementInfo> AchievementManager::ListAchievements() const {
  std::lock_guard lock(mutex_);
  return achievements_;
}

std::optional<AchievementInfo> AchievementManager::FindAchievement(uint32_t id) const {
  std::lock_guard lock(mutex_);
  auto it = std::find_if(achievements_.begin(), achievements_.end(),
                         [&](const AchievementInfo& info) { return info.id == id; });
  if (it == achievements_.end()) {
    return std::nullopt;
  }
  return *it;
}

AchievementUnlockResult AchievementManager::UnlockAchievement(
    uint32_t id, AchievementNotification notification) {
  AchievementEvent event;
  {
    std::lock_guard lock(mutex_);
    auto info = std::find_if(achievements_.begin(), achievements_.end(),
                             [&](const AchievementInfo& current) { return current.id == id; });
    if (info == achievements_.end()) {
      REXSYS_WARN("Achievement store: ignoring unknown achievement {:08X}", id);
      return AchievementUnlockResult::kUnknownAchievement;
    }

    uint64_t filetime = CurrentFileTime();
    auto [it, inserted] = unlocked_achievements_.emplace(id, filetime);
    if (!inserted) {
      return AchievementUnlockResult::kAlreadyUnlocked;
    }
    event = {*info, it->second};
  }

  REXSYS_INFO("Achievement unlocked: {:08X}", id);
  SaveUnlockState();
  DispatchCallbacks(unlock_callbacks_, event);
  if (notification == AchievementNotification::kShow) {
    DispatchCallbacks(notification_callbacks_, event);
  }
  return AchievementUnlockResult::kUnlocked;
}

bool AchievementManager::ShowAchievementNotification(uint32_t id) {
  AchievementEvent event;
  {
    std::lock_guard lock(mutex_);
    auto info = std::find_if(achievements_.begin(), achievements_.end(),
                             [&](const AchievementInfo& current) { return current.id == id; });
    if (info == achievements_.end()) {
      return false;
    }
    auto unlock = unlocked_achievements_.find(id);
    event = {*info, unlock != unlocked_achievements_.end() ? unlock->second : 0};
  }
  DispatchCallbacks(notification_callbacks_, event);
  return true;
}

bool AchievementManager::IsUnlocked(uint32_t id) const {
  std::lock_guard lock(mutex_);
  return unlocked_achievements_.contains(id);
}

uint64_t AchievementManager::GetUnlockTime(uint32_t id) const {
  std::lock_guard lock(mutex_);
  auto it = unlocked_achievements_.find(id);
  return it != unlocked_achievements_.end() ? it->second : 0;
}

AchievementListenerHandle AchievementManager::RegisterUnlockCallback(AchievementCallback callback) {
  if (!callback) {
    return 0;
  }
  std::lock_guard lock(mutex_);
  AchievementListenerHandle handle = next_listener_handle_++;
  unlock_callbacks_.emplace(handle, std::move(callback));
  return handle;
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
  unlock_callbacks_.erase(handle);
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
        uint32_t id = static_cast<uint32_t>(std::stoul(std::string(key.str())));
        uint64_t filetime = 0;
        if (const auto* entry = value.as_table()) {
          filetime = static_cast<uint64_t>((*entry)["filetime"].value_or<int64_t>(0));
        }
        unlocked_achievements_.emplace(id, filetime != 0 ? filetime : CurrentFileTime());
      }
    } else if (const auto* unlocked = table["unlocked"].as_array()) {
      migrated = true;
      uint64_t now = CurrentFileTime();
      for (const auto& node : *unlocked) {
        if (auto value = node.value<int64_t>()) {
          uint32_t id = static_cast<uint32_t>(*value);
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
  std::unordered_map<uint32_t, uint64_t> unlocked;
  {
    std::lock_guard lock(mutex_);
    save_path = unlock_save_path_;
    unlocked = unlocked_achievements_;
  }
  if (save_path.empty()) {
    return;
  }

  std::error_code ec;
  if (!save_path.parent_path().empty()) {
    std::filesystem::create_directories(save_path.parent_path(), ec);
  }
  if (ec) {
    REXSYS_WARN("Achievement save: cannot create {}: {}", save_path.parent_path().string(),
                ec.message());
    return;
  }

  std::string content = "# Achievement unlock state - managed by ReXGlue runtime\n\n";
  for (const auto& [id, filetime] : unlocked) {
    content += fmt::format("[unlocked.{}]\nfiletime = {}\n\n", id, filetime);
  }

  std::filesystem::path temporary_path = save_path;
  temporary_path += ".tmp";
  {
    std::ofstream file(temporary_path, std::ios::binary);
    if (!file) {
      REXSYS_WARN("Achievement save: cannot write {}", temporary_path.string());
      return;
    }
    file << content;
  }

  std::filesystem::remove(save_path, ec);
  ec.clear();
  std::filesystem::rename(temporary_path, save_path, ec);
  if (ec) {
    REXSYS_WARN("Achievement save: rename failed: {}", ec.message());
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

}  // namespace rex::system
