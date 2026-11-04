#include "hid/input_module.h"

#include <ranges>

#include <rex/logging/log_category.h>
#include <rex/logging/logger.h>
#include <rex/settings_registry.h>
#include <rex/string/text.h>
#include <rex/driver_module.h>

namespace rex::hid {

std::span<const InterfaceType> InputModule::GetRequiredInterfaces() const {
  static constexpr InterfaceType kRequired[] = {IDriverContext::kInterface, IAppInfo::kInterface,
                                                ISettingsRegistry::kInterface, ILogger::kInterface};
  return kRequired;
}

InputSystem& InputModule::GetOrCreateSystem() {
  if (!system_ || retired_) {
    system_ = std::make_unique<InputSystem>();
    retired_ = false;
  }
  return *system_;
}

Result<void> InputModule::Connect(IInterfaceFactory& factory) {
  GetOrCreateSystem();
  context_ = factory.Get<IDriverContext>();
  needs_ = &factory.Get<IAppInfo>()->GetRequirements();
  settings_ = factory.Get<ISettingsRegistry>();
  logger_ = factory.Get<ILogger>();
  factory_ = &factory;
  REX_TRY_VOID(logger_->AddCategories(log::kInputLog));
  if (auto registered = settings_->Register(system_->GetSettings()); !registered) {
    logger_->RemoveCategories(log::kInputLog);
    return registered;
  }
  if (auto registered = system_->RegisterBindings(*settings_); !registered) {
    settings_->Unregister(system_->GetSettings());
    logger_->RemoveCategories(log::kInputLog);
    return registered;
  }
  return Ok();
}

void InputModule::Disconnect() {
  system_->UnregisterBindings();
  settings_->Unregister(system_->GetSettings());
  logger_->RemoveCategories(log::kInputLog);
  retired_ = true;
  settings_ = nullptr;
  logger_ = nullptr;
  needs_ = nullptr;
  context_ = nullptr;
  factory_ = nullptr;
}

Result<void> InputModule::Init() {
  const std::string wanted = system_->GetSettings().drivers.Get();
  for (const auto part : std::views::split(std::string_view(wanted), ',')) {
    const std::string_view driver_name = string::Trim(std::string_view(part.begin(), part.end()));
    if (driver_name.empty()) {
      continue;
    }
    auto selected = SelectDrivers(*factory_, DriverType::kInput, driver_name, *context_);
    if (!selected) {
      REXHID_WARN("input drivers: {}", selected.error().message);
      continue;
    }
    for (std::unique_ptr<IDriver>& driver : *selected) {
      drivers_.push_back(std::move(driver));
    }
  }
  std::vector<rex::hid::IInputDriver*> started;
  for (const std::unique_ptr<IDriver>& driver : drivers_) {
    started.push_back(static_cast<rex::hid::IInputDriver*>(driver.get()));
  }
  if (started.empty() && needs_->input) {
    return Err(ErrorCategory::kConfig, "no input driver started, and the app needs input");
  }
  if (auto result = system_->Start(started); !result) {
    Shutdown();
    return result;
  }
  return Ok();
}

void InputModule::Shutdown() {
  if (system_) {
    system_->Stop();
  }
  for (auto it = drivers_.rbegin(); it != drivers_.rend(); ++it) {
    (*it)->Shutdown();
  }
  drivers_.clear();
}

IInterface* InputModule::QueryInterface(std::string_view name, u32 version) {
  return FindModuleInterface<rex::hid::IInputSystem>(GetOrCreateSystem(), name, version);
}

}  // namespace rex::hid
