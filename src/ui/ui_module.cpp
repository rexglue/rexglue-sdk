#include "ui/ui_module.h"

#include <rex/apu/audio_system.h>
#include <rex/gpu/graphics_system.h>
#include <rex/hid/input_system.h>
#include <rex/logging/log.h>
#include <rex/logging/logger.h>
#include <rex/setting.h>
#include <rex/settings.h>
#include <rex/ui/ui_driver.h>
#include <rex/version.h>
#include <rex/driver_module.h>
#include <rex/ui/ui_thread.h>

namespace rex::ui {

std::span<const InterfaceType> UIModule::GetRequiredInterfaces() const {
  static constexpr InterfaceType kRequired[] = {IDriverContext::kInterface, IAppInfo::kInterface,
                                                ISettings::kInterface, ILogger::kInterface};
  return kRequired;
}

UISystem& UIModule::GetOrCreateSystem() {
  if (!system_ || retired_) {
    system_ = std::make_unique<UISystem>();
    retired_ = false;
  }
  return *system_;
}

Result<void> UIModule::Connect(IFactory& factory) {
  GetOrCreateSystem();
  context_ = factory.Get<IDriverContext>();
  info_ = factory.Get<IAppInfo>();
  settings_ = factory.Get<ISettings>();
  logger_ = factory.Get<ILogger>();
  needs_ = &info_->GetRequirements();
  factory_ = &factory;
  REX_TRY_VOID(logger_->AddCategories(log::kUILog));
  if (auto registered = settings_->Register(system_->GetSettings()); !registered) {
    logger_->RemoveCategories(log::kUILog);
    return registered;
  }
  return Ok();
}

void UIModule::Disconnect() {
  settings_->Unregister(system_->GetSettings());
  logger_->RemoveCategories(log::kUILog);
  retired_ = true;
  info_ = nullptr;
  settings_ = nullptr;
  logger_ = nullptr;
  needs_ = nullptr;
  context_ = nullptr;
  factory_ = nullptr;
}

Result<void> UIModule::Init() {
  rex::ui::IUIDriver* driver = nullptr;
  if (needs_->ui || system_->GetSettings().driver.Get() != "none") {
    auto selected =
        SelectDrivers(*factory_, DriverType::kUI, system_->GetSettings().driver.Get(), *context_);
    if (selected && !selected->empty()) {
      driver_ = std::move(selected->front());
      driver = static_cast<rex::ui::IUIDriver*>(driver_.get());
    } else if (!selected) {
      REXLOG_WARN(ui, "ui driver: {}", selected.error().message);
    }
  }
  if (!driver && needs_->ui) {
    return Err(ErrorCategory::kConfig, "no UI driver is available");
  }
  rex::hid::IInputSystem* input = factory_->Get<rex::hid::IInputSystem>();
  rex::apu::IAudioSystem* audio = factory_->Get<rex::apu::IAudioSystem>();
  rex::gpu::IGraphicsSystem* graphics = factory_->Get<rex::gpu::IGraphicsSystem>();
  if (!audio || !graphics) {
    Shutdown();
    return Err(ErrorCategory::kModule, "the UI module finds no audio or graphics system");
  }
  if (system_->GetSettings().title.Get().empty() && driver && driver->HasDisplay()) {
    (void)system_->GetSettings().title.SetText(
        SettingLayer::kTitle,
        std::string(info_->GetAppName()) + " " + std::string(REXGLUE_BUILD_LABEL));
  }
  if (auto started = system_->Start(driver, input, *audio, *graphics, *settings_, *logger_);
      !started) {
    Shutdown();
    return started;
  }
  if (needs_->ui && system_->IsHeadless()) {
    Shutdown();
    return Err(ErrorCategory::kConfig,
               "the app needs a window, but the UI driver has no display or the window did not "
               "open");
  }
  return Ok();
}

void UIModule::Shutdown() {
  if (system_) {
    system_->Stop();
  }
  if (driver_) {
    driver_->Shutdown();
    driver_.reset();
  }
}

IInterface* UIModule::QueryInterface(std::string_view name, u32 version) {
  return FindModuleInterface<rex::ui::IUISystem, IUIControl, IUIThread>(GetOrCreateSystem(), name,
                                                                        version);
}

}  // namespace rex::ui
