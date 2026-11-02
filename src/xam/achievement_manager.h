/**
 * @file        xam/achievement_manager.h
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include <rex/platform.h>
#include <rex/types.h>
#include <rex/xam/achievement_manager.h>

namespace rex::xam {

/**
 * Finds the unlock save of a title under the user data folder.
 *
 * @param user_root The user data folder.
 * @return The save path, using the folder and file names as stored on disk.
 */
std::filesystem::path ResolveUnlockSavePath(const std::filesystem::path& user_root, u32 title_id);

class AchievementManager final : public rex::xam::IAchievementManager {
 public:
  [[nodiscard]] Result<void> RegisterAchievement(Achievement info) override;
  [[nodiscard]] Result<void> RegisterAchievements(
      std::span<const Achievement> achievements) override;
  void ReplaceAchievements(std::vector<Achievement> achievements) override;
  [[nodiscard]] Result<void> LoadMetadataFile(const std::filesystem::path& path) override;
  std::vector<Achievement> ListAchievements() const override;
  rex::xam::AchievementUnlockResult UnlockAchievement(
      u32 id, rex::xam::AchievementNotification notification =
                  rex::xam::AchievementNotification::kSuppress) override;
  bool IsUnlocked(u32 id) const override;
  std::optional<u64> GetUnlockTime(u32 id) const override;
  [[nodiscard]] ScopeGuard RegisterNotificationCallback(
      rex::xam::AchievementCallback callback) override;
  void SetUnlockSavePath(std::filesystem::path path) override;
  void LoadUnlockState() override;
  void LoadUnlockStateFrom(const std::filesystem::path& user_root, u32 title_id) override;
  void SaveUnlockState() const override;

 private:
  static u64 GetCurrentFileTime();

  void DispatchCallbacks(const std::unordered_map<u64, rex::xam::AchievementCallback>& callbacks,
                         const rex::xam::AchievementEvent& event) const;

  mutable std::mutex mutex_;
  mutable std::mutex save_mutex_;
  std::vector<Achievement> achievements_;
  std::unordered_map<u32, u64> unlocked_achievements_;
  std::filesystem::path unlock_save_path_;
  std::unordered_map<u64, rex::xam::AchievementCallback> notification_callbacks_;
  u64 next_listener_handle_ = 1;
};

}  // namespace rex::xam
