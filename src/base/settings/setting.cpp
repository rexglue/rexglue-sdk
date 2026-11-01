/**
 * @file        base/settings/setting.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <format>

#include "base/settings/settings_internal.h"

#include <rex/setting.h>
#include <rex/settings.h>
#include <rex/command.h>
#include <rex/utility.h>

namespace rex {

std::atomic<bool> g_startup_frozen{false};
std::atomic<bool> g_launch_frozen{false};

SettingBase::SettingBase(SettingOwner& owner, std::string_view name, SettingType type,
                         std::string_view help)
    : state_(std::make_unique<State>()) {
  state_->owner = &owner;
  state_->name = std::format("{}.{}", owner.GetScope(), name);
  state_->help = help;
  state_->type = type;
  owner.state_->settings.push_back(this);
}

SettingBase::~SettingBase() {
  SettingOwner::State& owner = *state_->owner->state_;
  if (owner.registered) {
    REX_SETTINGS().Detach(*this);
  }
  std::erase(owner.settings, this);
}

const std::string& SettingBase::GetName() const {
  return state_->name;
}

const std::string& SettingBase::GetHelp() const {
  return state_->help;
}

SettingType SettingBase::GetType() const {
  return state_->type;
}

const SettingConstraints& SettingBase::GetConstraints() const {
  return state_->constraints;
}

SettingOwner& SettingBase::GetOwner() const {
  return *state_->owner;
}

SettingLayer SettingBase::GetLayer() const {
  return state_->layer;
}

SettingConstraints& SettingBase::EditConstraints() {
  return state_->constraints;
}

bool SettingBase::IsFrozen() const {
  if (state_->layer == SettingLayer::kLock) {
    return true;
  }
  switch (state_->constraints.life) {
    case SettingLife::kStartupOnly:
      return g_startup_frozen.load();
    case SettingLife::kUntilLaunch:
      return g_launch_frozen.load();
    default:
      return false;
  }
}

bool SettingBase::IsRestartPending() const {
  return state_->restart_pending;
}

std::string SettingBase::GetText() const {
  return Render();
}

std::string SettingBase::GetDefaultText() const {
  return RenderDefault();
}

Result<void> SettingBase::Check(std::string_view text) const {
  const auto fail = [&](std::string_view why) {
    return Err(ErrorCategory::kConfig, std::format("{}: '{}' {}", state_->name, text, why));
  };
  f64 numeric = 0;
  bool is_numeric = true;
  switch (state_->type) {
    case SettingType::kBool: {
      bool v;
      is_numeric = false;
      if (!detail::ParseSetting(text, v))
        return fail("is not a boolean");
      break;
    }
    case SettingType::kI32: {
      i32 v;
      if (!detail::ParseSetting(text, v))
        return fail("is not a 32 bit integer");
      numeric = v;
      break;
    }
    case SettingType::kI64: {
      i64 v;
      if (!detail::ParseSetting(text, v))
        return fail("is not a 64 bit integer");
      numeric = static_cast<f64>(v);
      break;
    }
    case SettingType::kU32: {
      u32 v;
      if (!detail::ParseSetting(text, v))
        return fail("is not an unsigned 32 bit integer");
      numeric = v;
      break;
    }
    case SettingType::kU64: {
      u64 v;
      if (!detail::ParseSetting(text, v))
        return fail("is not an unsigned 64 bit integer");
      numeric = static_cast<f64>(v);
      break;
    }
    case SettingType::kF64: {
      f64 v;
      if (!detail::ParseSetting(text, v))
        return fail("is not a number");
      numeric = v;
      break;
    }
    default:
      is_numeric = false;
      break;
  }
  const auto& c = state_->constraints;
  if (is_numeric) {
    if (c.min && numeric < *c.min)
      return fail(std::format("is below {}", *c.min));
    if (c.max && numeric > *c.max)
      return fail(std::format("is above {}", *c.max));
  }
  if (!c.allowed.empty() && state_->type == SettingType::kString &&
      std::ranges::find(c.allowed, std::string(text)) == c.allowed.end()) {
    return fail("is not an accepted value");
  }
  return Ok();
}

Result<void> SettingBase::SetText(SettingLayer layer, std::string_view text) {
  if (state_->layer == SettingLayer::kLock) {
    return Err(ErrorCategory::kConfig, std::format("{} is locked", state_->name));
  }
  if (IsFrozen()) {
    return Err(ErrorCategory::kConfig, std::format("{} can no longer change", state_->name));
  }
  if (layer < state_->layer) {
    return Err(ErrorCategory::kConfig, std::format("{} is held by a higher layer", state_->name));
  }
  if (auto checked = Check(text); !checked) {
    return checked;
  }
  if (!Store(text)) {
    return Err(ErrorCategory::kConfig, std::format("{}: '{}' did not store", state_->name, text));
  }
  state_->layer = layer;
  if (state_->constraints.life == SettingLife::kRestart && g_startup_frozen.load()) {
    state_->restart_pending = true;
    return Ok();
  }
  state_->restart_pending = false;
  Publish();
  Notify();
  return Ok();
}

Result<void> SettingBase::SetFromCode(std::string_view text) {
  return SetText(g_launch_frozen.load() ? SettingLayer::kRuntime : SettingLayer::kTitle, text);
}

Result<void> SettingBase::Reset() {
  if (IsFrozen()) {
    return Err(ErrorCategory::kConfig, std::format("{} can no longer change", state_->name));
  }
  const std::string text = RenderDefault();
  if (!Store(text)) {
    return Err(ErrorCategory::kConfig, std::format("{}: default did not store", state_->name));
  }
  state_->layer = SettingLayer::kDefault;
  state_->restart_pending = false;
  Publish();
  Notify();
  return Ok();
}

SettingOwner::SettingOwner(std::string_view scope) : state_(std::make_unique<State>()) {
  state_->scope = scope;
}

SettingOwner::~SettingOwner() = default;

const std::string& SettingOwner::GetScope() const {
  return state_->scope;
}

std::span<SettingBase* const> SettingOwner::GetOwnedSettings() const {
  return state_->settings;
}

Command::Command(SettingOwner* owner, std::string_view name, std::string_view help,
                 MoveOnlyFunction<Result<void>(std::string_view)> run)
    : state_(std::make_unique<State>()) {
  state_->owner = owner;
  state_->name = std::format("{}.{}", owner->GetScope(), name);
  state_->help = help;
  state_->run = std::move(run);
  owner->state_->commands.push_back(this);
}

Command::~Command() {
  SettingOwner::State& owner = *state_->owner->state_;
  if (owner.registered) {
    REX_SETTINGS().Detach(*this);
  }
  std::erase(owner.commands, this);
}

const std::string& Command::GetName() const {
  return state_->name;
}

const std::string& Command::GetHelp() const {
  return state_->help;
}

Result<void> Command::Run(std::string_view args) {
  return state_->run(args);
}

}  // namespace rex
