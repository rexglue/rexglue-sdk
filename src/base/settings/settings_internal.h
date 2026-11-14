/**
 * @file        base/settings/settings_internal.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <array>
#include <string>
#include <vector>

#include <rex/setting.h>
#include <rex/settings_registry.h>
#include <rex/console_command.h>
#include <rex/utility.h>

namespace rex {

inline constexpr size_t kSettingLayerCount = 7;

void RegisterAliases(SettingsRegistry& settings);

extern std::atomic<bool> g_startup_frozen;
extern std::atomic<bool> g_launch_frozen;

struct SettingOwner::State {
  std::string scope;
  std::vector<SettingBase*> settings;
  std::vector<ConsoleCommand*> commands;
  bool registered = false;
};

struct SettingBase::State {
  SettingOwner* owner = nullptr;
  std::string name;
  std::string help;
  SettingType type = SettingType::kBool;
  SettingConstraints constraints;
  SettingLayer layer = SettingLayer::kDefault;
  bool restart_pending = false;
};

struct ConsoleCommand::State {
  SettingOwner* owner = nullptr;
  std::string name;
  std::string help;
  MoveOnlyFunction<Result<void>(std::string_view)> run;
};

struct SettingsRegistry::State {
  mutable std::recursive_mutex mutex;
  std::map<std::string, SettingBase*, std::less<>> settings;
  std::map<std::string, ConsoleCommand*, std::less<>> commands;
  std::map<std::string, std::string, std::less<>> aliases;
  std::map<std::string, std::array<std::optional<std::string>, kSettingLayerCount>, std::less<>>
      waiting;
  std::vector<std::string> notices;
  std::set<std::string, std::less<>> awaiting;
  bool env_active = false;
};

}  // namespace rex
