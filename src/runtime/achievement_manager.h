/**
 * @file        runtime/achievement_manager.h
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
#include <rex/export.h>
#include <rex/runtime/achievement_manager.h>

namespace rex::runtime {

/**
 * Finds the unlock save of a title under the user data folder.
 *
 * @param user_root The user data folder.
 * @param title_id The title ID.
 * @return The save path, naming the folder and file as they exist when they differ only in case.
 */
REX_RUNTIME_API std::filesystem::path ResolveUnlockSavePath(const std::filesystem::path& user_root,
                                                            u32 title_id);

/** The achievement manager of a runtime. */
class REX_RUNTIME_API AchievementManager final : public IAchievementManager {
 public:
  [[nodiscard]] Result<void> RegisterAchievement(AchievementInfo info) override;
  [[nodiscard]] Result<void> RegisterAchievements(
      std::span<const AchievementInfo> achievements) override;
  void ReplaceAchievements(std::vector<AchievementInfo> achievements) override;
  [[nodiscard]] Result<void> LoadMetadataFile(const std::filesystem::path& path) override;
  std::vector<AchievementInfo> ListAchievements() const override;
  AchievementUnlockResult UnlockAchievement(
      u32 id, AchievementNotification notification = AchievementNotification::kSuppress) override;
  bool IsUnlocked(u32 id) const override;
  std::optional<u64> GetUnlockTime(u32 id) const override;
  AchievementListenerHandle RegisterNotificationCallback(AchievementCallback callback) override;
  void UnregisterCallback(AchievementListenerHandle handle) override;
  void SetUnlockSavePath(std::filesystem::path path) override;
  void LoadUnlockState() override;
  void SaveUnlockState() const override;

 private:
  static u64 GetCurrentFileTime();

  void DispatchCallbacks(
      const std::unordered_map<AchievementListenerHandle, AchievementCallback>& callbacks,
      const AchievementEvent& event) const;

  mutable std::mutex mutex_;
  mutable std::mutex save_mutex_;
  std::vector<AchievementInfo> achievements_;
  std::unordered_map<u32, u64> unlocked_achievements_;
  std::filesystem::path unlock_save_path_;
  std::unordered_map<AchievementListenerHandle, AchievementCallback> notification_callbacks_;
  AchievementListenerHandle next_listener_handle_ = 1;
};

}  // namespace rex::runtime
