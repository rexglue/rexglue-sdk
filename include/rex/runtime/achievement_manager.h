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
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include <rex/platform.h>
#include <rex/result.h>
#include <rex/runtime/achievement_info.h>
#include <rex/types.h>
#include <rex/module.h>

namespace rex::runtime {

/** An unlock or notification event of an achievement. */
struct AchievementEvent {
  AchievementInfo achievement; /**< The achievement. */
};

/** Selects whether an unlock raises the notification callbacks. */
enum class AchievementNotification : u8 {
  kShow,     /**< Raise the notification callbacks. */
  kSuppress, /**< Persist the unlock only. */
};

/** The outcome of an unlock. */
enum class AchievementUnlockResult : u8 {
  kUnlocked,           /**< The achievement was locked and is now unlocked. */
  kAlreadyUnlocked,    /**< The achievement was already unlocked. */
  kUnknownAchievement, /**< No achievement has the id. */
};

/** The handle of a registered callback. */
using AchievementListenerHandle = u64;

/** A callback that receives achievement events. */
using AchievementCallback = std::function<void(const AchievementEvent&)>;

/** The achievement catalog, unlock persistence and unlock events of the running title. */
REX_INTERFACE(IAchievementManager, "rex.runtime.achievement_manager", 1)
 public:
  /**
   * Adds an achievement or replaces the one with the same id.
   *
   * @param info The achievement.
   * @return Ok, or a Validation error for id 0.
   */
  [[nodiscard]] virtual Result<void> RegisterAchievement(AchievementInfo info) = 0;

  /**
   * Adds or replaces several achievements, all or none.
   *
   * @param achievements The entries to add or replace.
   * @return Ok, or a Validation error for an id 0 entry, in which case nothing is registered.
   */
  [[nodiscard]] virtual Result<void> RegisterAchievements(
      std::span<const AchievementInfo> achievements) = 0;

  /**
   * Replaces the whole catalog.
   *
   * @param achievements The entries that replace the catalog.
   */
  virtual void ReplaceAchievements(std::vector<AchievementInfo> achievements) = 0;

  /**
   * Loads and merges the achievements entries of an editable TOML file.
   *
   * @param path The file.
   * @return Ok, or a Format error when the file has no achievements entries or does not parse.
   */
  [[nodiscard]] virtual Result<void> LoadMetadataFile(const std::filesystem::path& path) = 0;

  /**
   * Lists the catalog.
   *
   * @return The achievements in registration order.
   */
  [[nodiscard]] virtual std::vector<AchievementInfo> ListAchievements() const = 0;

  /**
   * Unlocks an achievement and persists the unlock.
   *
   * @param id The achievement id.
   * @param notification kShow to raise the notification callbacks too.
   * @return kUnlocked, kAlreadyUnlocked or kUnknownAchievement.
   */
  virtual AchievementUnlockResult UnlockAchievement(
      u32 id, AchievementNotification notification = AchievementNotification::kSuppress) = 0;

  /**
   * Reports whether an achievement is unlocked.
   *
   * @param id The achievement id.
   * @return True when it is unlocked.
   */
  [[nodiscard]] virtual bool IsUnlocked(u32 id) const = 0;

  /**
   * Returns when an achievement was unlocked.
   *
   * @param id The achievement id.
   * @return The FILETIME of the unlock, empty when it is locked.
   */
  [[nodiscard]] virtual std::optional<u64> GetUnlockTime(u32 id) const = 0;

  /**
   * Registers a callback for every notification.
   *
   * @param callback The function that receives each notification.
   * @return The handle UnregisterCallback takes.
   */
  virtual AchievementListenerHandle RegisterNotificationCallback(AchievementCallback callback) = 0;

  /**
   * Removes a callback.
   *
   * @param handle The handle a Register call returned.
   */
  virtual void UnregisterCallback(AchievementListenerHandle handle) = 0;

  /**
   * Sets the file unlocks persist to.
   *
   * @param path The file.
   */
  virtual void SetUnlockSavePath(std::filesystem::path path) = 0;

  /** Reads the unlocks from the save path. */
  virtual void LoadUnlockState() = 0;

  /** Writes the unlocks to the save path. */
  virtual void SaveUnlockState() const = 0;
};

}  // namespace rex::runtime
