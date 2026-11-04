/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include "xam/xam_module.h"

#include <format>

#include <rex/logging/logger.h>

#include "xam/xam_state.h"
#include "xam/xam_ui.h"
#include <rex/xam/title_lifecycle.h>
#include <rex/xam/language.h>

namespace rex {
namespace xam {

using namespace rex::kernel;

namespace {

XAMSettings g_settings;

template <typename T>
Result<Setting<T>*> FindSetting(ISettingsRegistry& settings, std::string_view name) {
  auto found = settings.Find(name);
  if (!found) {
    return Err<Setting<T>*>(ErrorCategory::kModule,
                            std::format("the setting {} is not registered", name));
  }
  if ((*found)->GetType() != ::rex::detail::TypeOf<T>()) {
    return Err<Setting<T>*>(ErrorCategory::kModule,
                            std::format("the setting {} has an unexpected type", name));
  }
  return static_cast<Setting<T>*>(*found);
}

}  // namespace

const XAMSettings& GetXAMSettings() {
  return g_settings;
}

std::string_view XAMModule::GetName() const {
  return "rexxam";
}

std::span<const InterfaceType> XAMModule::GetRequiredInterfaces() const {
  static constexpr InterfaceType kRequired[] = {ISettingsRegistry::kInterface, ILogger::kInterface};
  return kRequired;
}

Result<void> XAMModule::Connect(IInterfaceFactory& factory) {
  settings_ = factory.Get<ISettingsRegistry>();
  logger_ = factory.Get<ILogger>();
  return logger_->AddCategories(log::kXAMLog);
}

void XAMModule::Disconnect() {
  logger_->RemoveCategories(log::kXAMLog);
  logger_ = nullptr;
  settings_ = nullptr;
}

Result<void> XAMModule::Init() {
  XAMSettings found;
  auto headless = FindSetting<bool>(*settings_, "kernel.headless");
  if (!headless) {
    return Err(headless.error());
  }
  found.headless = *headless;
  auto user_country = FindSetting<u32>(*settings_, "kernel.user_country");
  if (!user_country) {
    return Err(user_country.error());
  }
  found.user_country = *user_country;
  auto license_mask = FindSetting<u32>(*settings_, "kernel.license_mask");
  if (!license_mask) {
    return Err(license_mask.error());
  }
  found.license_mask = *license_mask;
  g_settings = found;
  return {};
}

void XAMModule::Shutdown() {
  g_settings = {};
}

IInterface* XAMModule::QueryInterface(std::string_view name, u32 version) {
  if (name == IXAMTitleLifecycle::kInterface.name &&
      version == IXAMTitleLifecycle::kInterface.version) {
    return static_cast<IXAMTitleLifecycle*>(&events_);
  }
  if (name == IXAMTitleServices::kInterface.name &&
      version == IXAMTitleServices::kInterface.version) {
    return static_cast<IXAMTitleServices*>(&events_);
  }
  if (name == IXAMLanguage::kInterface.name && version == IXAMLanguage::kInterface.version) {
    return static_cast<IXAMLanguage*>(&language_);
  }
  if (name == IXAMExports::kInterface.name && version == IXAMExports::kInterface.version) {
    return static_cast<IXAMExports*>(&exports_);
  }
  return IModule::QueryInterface(name, version);
}

bool XAMIsUIActive() {
  XAMState* xam = FindCurrentXAM();
  return xam && xam->GetShownPrompts() > 0;
}

}  // namespace xam
}  // namespace rex
