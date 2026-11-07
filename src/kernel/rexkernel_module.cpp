#include "kernel/rexkernel_module.h"
#include <rex/xam/language.h>
#include <rex/xam/title_lifecycle.h>

#include <atomic>
#include <span>

#include <rex/apu/audio_system.h>
#include <rex/filesystem/file_system_factory.h>
#include <rex/gpu/graphics_system.h>
#include <rex/hid/input_system.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/title_paths.h>
#include <rex/kernel/runtime_settings.h>
#include <rex/logging/log_category.h>
#include <rex/logging/logger.h>
#include <rex/settings_registry.h>
#include <rex/ui/ui_system.h>

#include "kernel/categories.h"
#include "kernel/kernel_control.h"
#include "kernel/ppc_hooks.h"
#include "kernel/settings_access.h"
#include "kernel/xam/forward.h"
#include "kernel/xbdm/forward.h"

REXLOG_TABLE(kRuntimeLog, kernel, cpu, title)

namespace rex::kernel {

namespace {

KernelServices g_services;
std::atomic<bool> g_connected{false};

}  // namespace

const KernelServices* GetKernelServices() {
  return g_connected.load(std::memory_order_acquire) ? &g_services : nullptr;
}

std::string_view KernelModule::GetName() const {
  return "rexkernel";
}

std::span<const InterfaceType> KernelModule::GetRequiredInterfaces() const {
  static constexpr InterfaceType kRequired[] = {ISettingsRegistry::kInterface, ILogger::kInterface,
                                                filesystem::IFileSystemFactory::kInterface,
                                                ui::IUISystem::kInterface};
  return kRequired;
}

Result<void> KernelModule::Connect(IInterfaceFactory& factory) {
  factory_ = &factory;
  settings_ = factory.Get<ISettingsRegistry>();
  logger_ = factory.Get<ILogger>();
  REX_TRY_VOID(logger_->AddCategories(log::kRuntimeLog));
  if (auto registered = RegisterSettings(); !registered) {
    logger_->RemoveCategories(log::kRuntimeLog);
    return registered;
  }
  g_services = {};
  g_services.file_system = factory.Get<filesystem::IFileSystemFactory>();
  g_services.input = factory.Get<hid::IInputSystem>();
  g_services.audio = factory.Get<apu::IAudioSystem>();
  g_services.graphics = factory.Get<gpu::IGraphicsSystem>();
  g_services.ui = factory.Get<ui::IUISystem>();
  g_services.language = factory.Get<xam::IXAMLanguage>();
  g_services.xam = factory.Get<xam::IXAMTitleLifecycle>();
  g_services.managers = factory.Get<xam::IXAMTitleServices>();
  g_services.kernel_settings = &kernel_settings_;
  g_connected.store(true, std::memory_order_release);
  return {};
}

void KernelModule::Disconnect() {
  g_connected.store(false, std::memory_order_release);
  g_services = {};
  settings_->Unregister(GetTitlePaths());
  settings_->Unregister(kernel_settings_);
  settings_->Unregister(GetRuntimeSettings());
  logger_->RemoveCategories(log::kRuntimeLog);
  factory_ = nullptr;
  settings_ = nullptr;
  logger_ = nullptr;
}

Result<void> KernelModule::Init() {
  if (!g_services.controls) {
    return Err(ErrorCategory::kModule, "the kernel was given no controls");
  }
  g_services.graphics = factory_->Get<gpu::IGraphicsSystem>();
  if (auto* exports = g_services.controls->Get<xam::IXAMExports>()) {
    REX_TRY_VOID(BindXAMForwards(exports->GetBodies()));
    forwards_ = true;
  }
  if (auto* exports = g_services.controls->Get<::rex::xbdm::IXBDMExports>()) {
    REX_TRY_VOID(BindXBDMForwards(exports->GetBodies()));
    xbdm_forwards_ = true;
  }
  RegisterPPCHooks();
  hooks_ = true;
  return {};
}

void KernelModule::Shutdown() {
  if (forwards_) {
    UnbindXAMForwards();
    forwards_ = false;
  }
  if (xbdm_forwards_) {
    UnbindXBDMForwards();
    xbdm_forwards_ = false;
  }
  if (hooks_) {
    UnregisterPPCHooks();
    hooks_ = false;
  }
}

IInterface* KernelModule::QueryInterface(std::string_view name, u32 version) {
  return FindModuleInterface<IKernelControl>(*this, name, version);
}

void KernelModule::SetControls(IInterfaceFactory& controls) {
  g_services.controls = &controls;
}

Result<void> KernelModule::RegisterSettings() {
  if (auto added = settings_->Register(GetRuntimeSettings()); !added) {
    return added;
  }
  if (auto added = settings_->Register(kernel_settings_); !added) {
    settings_->Unregister(GetRuntimeSettings());
    return added;
  }
  if (auto added = settings_->Register(GetTitlePaths()); !added) {
    settings_->Unregister(kernel_settings_);
    settings_->Unregister(GetRuntimeSettings());
    return added;
  }
  return {};
}

}  // namespace rex::kernel
