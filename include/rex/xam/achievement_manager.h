/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @file        xam/achievement_manager.h
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 *
 * @remarks     Parts derived from Xenia (Ben Vanik, 2020).
 * @remarks     Parts derived from Xenia (Ben Vanik, 2022).
 */

#pragma once

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>
#include <format>
#include <string>
#include <filesystem>
#include <optional>
#include <string_view>
#include <functional>

#include <rex/utility.h>
#include <span>

#include <rex/platform.h>
#include <rex/types.h>
#include <rex/memory.h>
#include <rex/kernel/types/status.h>
#include <rex/memory/stream.h>
#include <rex/kernel/types/content.h>
#include <rex/result.h>
#include <rex/filesystem/xdbf.h>
#include <rex/utility.h>
#include <rex/title.h>

namespace rex::xam {

struct AchievementEvent {
  Achievement achievement;
};

/** Selects whether an unlock raises the notification callbacks. */
enum class AchievementNotification : u8 {
  kShow,
  kSuppress, /**< Persist the unlock only. */
};

enum class AchievementUnlockResult : u8 {
  kUnlocked,
  kAlreadyUnlocked,
  kUnknownAchievement,
};

using AchievementCallback = std::function<void(const AchievementEvent&)>;

class IAchievementManager : public NonCopyable {
 public:
  virtual ~IAchievementManager() = default;

  /**
   * Adds an achievement or replaces the one with the same ID.
   *
   * @return Ok, or a Validation error for ID 0.
   */
  [[nodiscard]] virtual Result<void> RegisterAchievement(Achievement info) = 0;

  /**
   * Adds or replaces several achievements, all or none.
   *
   * @param achievements The entries to add or replace.
   * @return Ok, or a Validation error for an ID 0 entry, in which case nothing is registered.
   */
  [[nodiscard]] virtual Result<void> RegisterAchievements(
      std::span<const Achievement> achievements) = 0;

  /**
   * Replaces the whole catalog.
   *
   * @param achievements The entries that replace the catalog.
   */
  virtual void ReplaceAchievements(std::vector<Achievement> achievements) = 0;

  /**
   * Loads and merges the achievement entries of a TOML file.
   *
   * @return Ok, or a Format error when the file has no achievement entries or does not parse.
   */
  [[nodiscard]] virtual Result<void> LoadMetadataFile(const std::filesystem::path& path) = 0;

  /**
   * Lists the catalog.
   *
   * @return The achievements in registration order.
   */
  [[nodiscard]] virtual std::vector<Achievement> ListAchievements() const = 0;

  /**
   * Unlocks an achievement and persists the unlock.
   *
   * @param notification kShow to raise the notification callbacks too.
   * @return kUnlocked, kAlreadyUnlocked or kUnknownAchievement.
   */
  virtual AchievementUnlockResult UnlockAchievement(
      u32 id, AchievementNotification notification = AchievementNotification::kSuppress) = 0;

  [[nodiscard]] virtual bool IsUnlocked(u32 id) const = 0;

  /** Returns when an achievement was unlocked, empty when it is locked. */
  [[nodiscard]] virtual std::optional<u64> GetUnlockTime(u32 id) const = 0;

  /**
   * Registers a callback for every notification.
   *
   * @param callback The function that receives each notification.
   * @return The guard. Destroying it removes the callback. It does nothing for an empty callback.
   */
  [[nodiscard]] virtual ScopeGuard RegisterNotificationCallback(AchievementCallback callback) = 0;

  virtual void SetUnlockSavePath(std::filesystem::path path) = 0;

  virtual void LoadUnlockState() = 0;

  /** Sets the save path of a title under a user data root and reads the unlocks from it. */
  virtual void LoadUnlockStateFrom(const std::filesystem::path& user_root, u32 title_id) = 0;

  virtual void SaveUnlockState() const = 0;
};

}  // namespace rex::xam
