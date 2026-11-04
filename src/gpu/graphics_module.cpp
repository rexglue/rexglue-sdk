#include "gpu/graphics_module.h"

#include <format>

#include <rex/logging/logger.h>
#include <rex/settings_registry.h>
#include <rex/ui/ui_system.h>
#include <rex/driver_module.h>

namespace rex::gpu {

std::span<const InterfaceType> GraphicsModule::GetRequiredInterfaces() const {
  static constexpr InterfaceType kRequired[] = {IDriverContext::kInterface, IAppInfo::kInterface,
                                                ISettingsRegistry::kInterface, ILogger::kInterface};
  return kRequired;
}

GraphicsSystem& GraphicsModule::GetOrCreateSystem() {
  if (!system_ || retired_) {
    system_ = std::make_unique<GraphicsSystem>();
    retired_ = false;
  }
  return *system_;
}

Result<void> GraphicsModule::Connect(IInterfaceFactory& factory) {
  GetOrCreateSystem();
  context_ = factory.Get<IDriverContext>();
  needs_ = &factory.Get<IAppInfo>()->GetRequirements();
  settings_ = factory.Get<ISettingsRegistry>();
  logger_ = factory.Get<ILogger>();
  factory_ = &factory;
  REX_TRY_VOID(logger_->AddCategories(log::kGraphicsLog));
  if (auto registered = settings_->Register(system_->GetSettings()); !registered) {
    logger_->RemoveCategories(log::kGraphicsLog);
    return registered;
  }
  return Ok();
}

void GraphicsModule::Disconnect() {
  settings_->Unregister(system_->GetSettings());
  logger_->RemoveCategories(log::kGraphicsLog);
  retired_ = true;
  settings_ = nullptr;
  logger_ = nullptr;
  needs_ = nullptr;
  context_ = nullptr;
  factory_ = nullptr;
}

Result<void> GraphicsModule::Init() {
  if (needs_->graphics_family == GraphicsFamily::kNone) {
    return Ok();
  }
  rex::ui::IUISystem* ui = factory_->Get<rex::ui::IUISystem>();
  if (!ui) {
    return Err(ErrorCategory::kModule, "the graphics module finds no UI system");
  }
  const std::string driver_name = system_->GetDriverName();
  auto selected = SelectDrivers(*factory_, DriverType::kGraphics, driver_name, *context_);
  if (!selected) {
    return std::unexpected(selected.error());
  }
  if (selected->empty()) {
    if (needs_->graphics_family == GraphicsFamily::kXenos) {
      return Err(ErrorCategory::kConfig,
                 std::format("gpu.driver '{}' selects no graphics driver, and the app needs the "
                             "xenos family",
                             driver_name));
    }
    return Ok();
  }
  driver_ = std::move(selected->front());
  auto* driver = static_cast<rex::gpu::IGraphicsDriver*>(driver_.get());
  if (needs_->graphics_family == GraphicsFamily::kXenos && driver->AsXenos() == nullptr) {
    driver_->Shutdown();
    driver_.reset();
    return Err(ErrorCategory::kConfig,
               std::format("the graphics driver '{}' is not a Xenos driver, and the app needs "
                           "the xenos family",
                           driver_name));
  }
  if (auto window = ui->GetNativeWindow()) {
    if (auto surface = driver->SetSurface(*window); !surface) {
      driver_->Shutdown();
      driver_.reset();
      return surface;
    }
  }
  if (auto started = system_->Start(driver); !started) {
    driver_->Shutdown();
    driver_.reset();
    return std::unexpected(started.error());
  }
  system_->SetOverlayHook([ui](IOverlayRenderer& renderer) { ui->DrawOverlays(renderer); });
  return Ok();
}

void GraphicsModule::Shutdown() {
  if (system_) {
    system_->Stop();
  }
  if (driver_) {
    driver_->Shutdown();
    driver_.reset();
  }
}

IInterface* GraphicsModule::QueryInterface(std::string_view name, u32 version) {
  return FindModuleInterface<rex::gpu::IGraphicsSystem, IGraphicsControl>(GetOrCreateSystem(), name,
                                                                          version);
}

}  // namespace rex::gpu

REX_DECLARE_MODULE(rex::gpu::GraphicsModule)
