/**
 * @file        base/settings/settings_registry.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <cctype>
#include <format>

#include <rex/setting.h>
#include <rex/settings_registry.h>
#include <rex/console_command.h>
#include <rex/platform/process.h>

#include "base/settings/settings_internal.h"

namespace rex {

namespace {

std::string ToUpperEnv(std::string_view name) {
  std::string out = "REX_";
  for (char c : name) {
    out += (c == '.') ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return out;
}

}  // namespace

SettingsRegistry::SettingsRegistry() : state_(std::make_unique<State>()) {
  RegisterAliases(*this);
}

SettingsRegistry::~SettingsRegistry() = default;

SettingsRegistry& GetSettingsRegistry() {
  static SettingsRegistry* settings = new SettingsRegistry();
  return *settings;
}

Result<void> SettingsRegistry::Register(SettingOwner& owner) {
  std::lock_guard lock(state_->mutex);
  for (SettingBase* setting : owner.state_->settings) {
    if (auto alias = state_->aliases.find(setting->GetName()); alias != state_->aliases.end()) {
      return Err(
          ErrorCategory::kConfig,
          alias->second.empty()
              ? std::format("{} cannot be registered, it is a removed name", setting->GetName())
              : std::format("{} cannot be registered, it is an alias of {}", setting->GetName(),
                            alias->second));
    }
    if (auto it = state_->settings.find(setting->GetName()); it != state_->settings.end()) {
      return Err(ErrorCategory::kConfig,
                 std::format("{} is registered by '{}' and '{}'", setting->GetName(),
                             it->second->GetOwner().GetScope(), owner.GetScope()));
    }
  }
  for (ConsoleCommand* command : owner.state_->commands) {
    if (state_->commands.contains(command->GetName())) {
      return Err(ErrorCategory::kConfig, std::format("{} is registered twice", command->GetName()));
    }
  }
  for (SettingBase* setting : owner.state_->settings) {
    state_->settings.emplace(setting->GetName(), setting);
  }
  for (ConsoleCommand* command : owner.state_->commands) {
    state_->commands.emplace(command->GetName(), command);
  }
  owner.state_->registered = true;

  for (SettingBase* setting : owner.state_->settings) {
    const std::string& name = setting->GetName();
    if (auto waiting = state_->waiting.find(name); waiting != state_->waiting.end()) {
      auto layers = waiting->second;
      state_->waiting.erase(waiting);
      state_->awaiting.erase(name);
      for (size_t i = 0; i < kSettingLayerCount; ++i) {
        const auto& layer = layers[i];
        if (!layer)
          continue;
        if (setting->GetLayer() > static_cast<SettingLayer>(i))
          continue;
        if (auto r = setting->SetText(static_cast<SettingLayer>(i), *layer); !r) {
          state_->notices.push_back(r.error().GetDescription());
        }
      }
    }
    if (!state_->env_active)
      continue;
    if (auto env = platform::GetEnvironmentValue(ToUpperEnv(name))) {
      if (setting->GetLayer() <= SettingLayer::kEnvironment) {
        if (auto r = setting->SetText(SettingLayer::kEnvironment, *env); !r) {
          state_->notices.push_back(r.error().GetDescription());
        }
      }
    }
  }
  return Ok();
}

void SettingsRegistry::Detach(SettingBase& setting) {
  std::lock_guard lock(state_->mutex);
  state_->settings.erase(setting.GetName());
}

void SettingsRegistry::Detach(ConsoleCommand& command) {
  std::lock_guard lock(state_->mutex);
  state_->commands.erase(command.GetName());
}

void SettingsRegistry::ApplyEnvironment() {
  std::lock_guard lock(state_->mutex);
  state_->env_active = true;
  for (auto& [name, setting] : state_->settings) {
    if (auto env = platform::GetEnvironmentValue(ToUpperEnv(name))) {
      if (setting->GetLayer() > SettingLayer::kEnvironment)
        continue;
      if (auto r = setting->SetText(SettingLayer::kEnvironment, *env); !r) {
        state_->notices.push_back(r.error().GetDescription());
      }
    }
  }
}

void SettingsRegistry::Unregister(SettingOwner& owner) {
  std::lock_guard lock(state_->mutex);
  for (SettingBase* setting : owner.state_->settings) {
    if (setting->GetLayer() != SettingLayer::kDefault &&
        setting->GetLayer() != SettingLayer::kLock) {
      state_->waiting[setting->GetName()][static_cast<size_t>(setting->GetLayer())] =
          setting->GetText();
    }
    state_->settings.erase(setting->GetName());
  }
  for (ConsoleCommand* command : owner.state_->commands) {
    state_->commands.erase(command->GetName());
  }
  owner.state_->registered = false;
}

Result<SettingBase*> SettingsRegistry::Find(std::string_view name) const {
  std::lock_guard lock(state_->mutex);
  if (auto it = state_->settings.find(name); it != state_->settings.end()) {
    return it->second;
  }
  return Err<SettingBase*>(ErrorCategory::kNotFound, std::format("no setting named {}", name));
}

std::vector<SettingBase*> SettingsRegistry::Enumerate(std::string_view scope) const {
  std::lock_guard lock(state_->mutex);
  std::vector<SettingBase*> out;
  const std::string prefix = scope.empty() ? std::string() : std::string(scope) + ".";
  for (auto& [name, setting] : state_->settings) {
    if (prefix.empty() || name.starts_with(prefix))
      out.push_back(setting);
  }
  return out;
}

std::vector<ConsoleCommand*> SettingsRegistry::EnumerateCommands(std::string_view scope) const {
  std::lock_guard lock(state_->mutex);
  std::vector<ConsoleCommand*> out;
  const std::string prefix = scope.empty() ? std::string() : std::string(scope) + ".";
  for (auto& [name, command] : state_->commands) {
    if (prefix.empty() || name.starts_with(prefix))
      out.push_back(command);
  }
  return out;
}

bool SettingsRegistry::ApplyOldLogFile(SettingLayer layer, std::string_view name,
                                       std::string_view text, Result<void>& result) {
  bool flag = false;
  if (name != "log.file" || detail::ParseSetting(text, flag)) {
    return false;
  }
  result = Ok();
  if (text.empty()) {
    return true;
  }
  {
    std::lock_guard lock(state_->mutex);
    state_->notices.push_back(
        std::format("log.file {} is now log.file, app.log_root and log.file_name", text));
  }
  const std::filesystem::path path{std::string(text)};
  result = SetText(layer, "log.file", "true");
  if (result && path.has_parent_path()) {
    result = SetText(layer, "app.log_root", path.parent_path().string());
  }
  if (result) {
    result = SetText(layer, "log.file_name", path.filename().string());
  }
  return true;
}

Result<void> SettingsRegistry::SetText(SettingLayer layer, std::string_view name,
                                       std::string_view text) {
  if (Result<void> converted = Ok(); ApplyOldLogFile(layer, name, text, converted)) {
    return converted;
  }
  std::lock_guard lock(state_->mutex);
  std::string resolved(name);
  if (auto alias = state_->aliases.find(name); alias != state_->aliases.end()) {
    if (alias->second.empty()) {
      state_->notices.push_back(std::format("{} was removed and is ignored", name));
      return Ok();
    }
    resolved = alias->second;
    state_->notices.push_back(std::format("{} is now {}", name, resolved));
  }
  if (auto it = state_->settings.find(resolved); it != state_->settings.end()) {
    SettingBase* setting = it->second;
    if (layer < setting->GetLayer()) {
      return Ok();
    }
    return setting->SetText(layer, text);
  }
  state_->waiting[resolved][static_cast<size_t>(layer)] = std::string(text);
  state_->awaiting.insert(resolved);
  return Ok();
}

Result<void> SettingsRegistry::SetText(std::string_view name, std::string_view text) {
  if (Result<void> converted = Ok();
      ApplyOldLogFile(SettingLayer::kRuntime, name, text, converted)) {
    return converted;
  }
  std::lock_guard lock(state_->mutex);
  std::string resolved(name);
  if (auto alias = state_->aliases.find(name); alias != state_->aliases.end()) {
    if (alias->second.empty()) {
      state_->notices.push_back(std::format("{} was removed and is ignored", name));
      return Ok();
    }
    resolved = alias->second;
    state_->notices.push_back(std::format("{} is now {}", name, resolved));
  }
  auto found = Find(resolved);
  if (!found) {
    return std::unexpected(found.error());
  }
  return (*found)->SetText(SettingLayer::kRuntime, text);
}

void SettingsRegistry::AddAlias(std::string_view old_name, std::string_view new_name) {
  std::lock_guard lock(state_->mutex);
  state_->aliases[std::string(old_name)] = std::string(new_name);
}

bool SettingsRegistry::IsAlias(std::string_view name) const {
  std::lock_guard lock(state_->mutex);
  return state_->aliases.contains(name);
}

void SettingsRegistry::EndSession() {
  std::lock_guard lock(state_->mutex);
  for (auto& [name, setting] : state_->settings) {
    if (setting->Store(setting->RenderDefault())) {
      setting->state_->layer = SettingLayer::kDefault;
      setting->state_->restart_pending = false;
      setting->Publish();
      setting->Notify();
    }
  }
  state_->waiting.clear();
  state_->awaiting.clear();
  state_->notices.clear();
  state_->env_active = false;
  g_startup_frozen = false;
  g_launch_frozen = false;
}

std::vector<std::string> SettingsRegistry::TakeNotices() {
  std::lock_guard lock(state_->mutex);
  return std::exchange(state_->notices, {});
}

std::vector<UnknownSetting> SettingsRegistry::TakeUnknown() {
  std::lock_guard lock(state_->mutex);
  std::vector<UnknownSetting> out;
  for (const std::string& name : std::exchange(state_->awaiting, {})) {
    const auto waiting = state_->waiting.find(name);
    if (waiting == state_->waiting.end()) {
      continue;
    }
    for (size_t i = kSettingLayerCount; i > 0; --i) {
      if (waiting->second[i - 1]) {
        out.push_back({name, static_cast<SettingLayer>(i - 1)});
        break;
      }
    }
  }
  return out;
}

Result<void> SettingsRegistry::Reset(std::string_view name) {
  auto found = Find(name);
  if (!found) {
    return std::unexpected(found.error());
  }
  return (*found)->Reset();
}

Result<void> SettingsRegistry::Run(std::string_view name, std::string_view args) {
  ConsoleCommand* command = nullptr;
  {
    std::lock_guard lock(state_->mutex);
    if (auto it = state_->commands.find(name); it != state_->commands.end())
      command = it->second;
  }
  if (!command) {
    return Err(ErrorCategory::kNotFound, std::format("no command named {}", name));
  }
  return command->Run(args);
}

std::vector<std::string> SettingsRegistry::GetRestartPending() const {
  std::lock_guard lock(state_->mutex);
  std::vector<std::string> out;
  for (auto& [name, setting] : state_->settings) {
    if (setting->IsRestartPending())
      out.push_back(name);
  }
  return out;
}

}  // namespace rex
