/**
 * @file        base/settings/settings_control.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>
#include <vector>

#include <rex/module.h>
#include <rex/setting.h>
#include <rex/settings.h>

namespace rex {

/** Closes and opens writes for a setting lifetime. */
REX_DECLARE_PRIVATE_INTERFACE(ISettingsControl, 1)
/**
 * Closes writes for a setting lifetime. kStartupOnly closes when the modules connect, kUntilLaunch
 * when the title launches.
 *
 * @param life kStartupOnly or kUntilLaunch.
 */
  virtual void Freeze(SettingLife life) = 0;

  /**
   * Opens writes for a setting lifetime after a title unloads.
   *
   * @param life kUntilLaunch.
   */
  virtual void Thaw(SettingLife life) = 0;

  /** Applies REX_<SCOPE>_<NAME> variables to every setting, including those registered later. */
  virtual void ApplyEnvironment() = 0;

  /**
   * Returns the messages the registry produced, such as use of an old setting name, and clears
   * them, oldest first.
   */
  virtual std::vector<std::string> TakeNotices() = 0;

  /**
   * Returns the names a source set before any owner registered them and none has since, sorted,
   * with the highest layer of each.
   */
  virtual std::vector<UnknownSetting> TakeUnknown() = 0;

  /** Ends the setting lifetimes the app used, so a process can run another app. */
  virtual void EndSession() = 0;
};

}  // namespace rex
