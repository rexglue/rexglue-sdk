/**
 * @file        base/process_control.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include "base/logger_control.h"
#include "base/settings/settings_control.h"

namespace rex {

/** Reaches the private members of the process settings. */
class ProcessSettingsControl final : public ISettingsRegistryControl {
 public:
  void Freeze(SettingLife life) override;
  void Thaw(SettingLife life) override;
  void ApplyEnvironment() override;
  std::vector<std::string> TakeNotices() override;
  std::vector<UnknownSetting> TakeUnknown() override;
  void EndSession() override;
};

/** Reaches the private members of the process logger. */
class ProcessLoggerControl final : public ILoggerControl {
 public:
  void ApplySettled() override;
  [[nodiscard]] Result<void> OpenFile(const std::filesystem::path& log_root,
                                      std::string_view app_name) override;
  void EndSession() override;
};

ProcessSettingsControl& GetProcessSettingsControl();

ProcessLoggerControl& GetProcessLoggerControl();

}  // namespace rex
