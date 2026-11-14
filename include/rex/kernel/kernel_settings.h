/**
 * @file        kernel/kernel_settings.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>

#include <rex/platform.h>
#include <rex/setting.h>
#include <rex/types.h>

namespace rex::kernel {

class REX_KERNEL_API KernelSettings final : public SettingOwner {
 public:
  REX_SETTING(bool, headless, false, "System dialogs answer at once without showing");
  REX_SETTING(u32, user_language, 1, "XLanguage ID, 0 follows the host language", kUntilLaunch);
  REX_SETTING(u32, user_country, 103, "Console country code", kUntilLaunch);
  REX_SETTING(u32, license_mask, 0, "Content license mask", kUntilLaunch);
  REX_SETTING(std::string, command_line, "", "Arguments the title sees after default.xex",
              kUntilLaunch);

  KernelSettings();
};

}  // namespace rex::kernel
