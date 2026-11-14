/**
 * @file        kernel/rexkernel_module.h
 * @brief       The kernel as a module, and the services it connected to
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <span>
#include <string_view>

#include <rex/kernel/module.h>
#include <rex/kernel/object/notification_broadcaster.h>
#include <rex/kernel/object/object_table.h>
#include <rex/kernel/thread_registry.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/kernel_settings.h>
#include <rex/module.h>
#include <rex/settings_registry.h>

#include "kernel/kernel_control.h"

namespace rex {
class ILogger;
class ISettingsRegistry;
namespace filesystem {
class IFileSystemFactory;
}  // namespace filesystem
namespace hid {
class IInputSystem;
}  // namespace hid
namespace apu {
class IAudioSystem;
}  // namespace apu
namespace gpu {
class IGraphicsSystem;
}  // namespace gpu
namespace ui {
class IUISystem;
}  // namespace ui
namespace xam {
class IXAMLanguage;
class IXAMTitleLifecycle;
class IXAMTitleServices;
}  // namespace xam
}  // namespace rex

namespace rex::kernel {

/** The services the kernel module connected to. A service its module does not provide is null. */
struct KernelServices {
  filesystem::IFileSystemFactory* file_system = nullptr;
  hid::IInputSystem* input = nullptr;
  apu::IAudioSystem* audio = nullptr;
  gpu::IGraphicsSystem* graphics = nullptr;
  ui::IUISystem* ui = nullptr;
  xam::IXAMLanguage* language = nullptr;
  xam::IXAMTitleLifecycle* xam = nullptr;
  xam::IXAMTitleServices* managers = nullptr;
  IInterfaceFactory* controls =
      nullptr; /**< The private factory the app set, null until it is set. */
  KernelSettings* kernel_settings = nullptr;
};

/** Returns the services of the running kernel module, or null when the module is not connected. */
const KernelServices* GetKernelServices();

/** The kernel module. The program registers one with the app or loads it from rexkernel. */
class KernelModule final : public IModule, public IKernelControl {
 public:
  REX_MODULE_SERVES(::rex::kernel::IKernelControl, ::rex::kernel::IObjectTable,
                    ::rex::kernel::IThreadRegistry, ::rex::kernel::IModuleRegistry,
                    ::rex::kernel::INotificationBroadcaster, ::rex::kernel::IOverlappedDispatcher)

  std::string_view GetName() const override;
  std::span<const InterfaceType> GetRequiredInterfaces() const override;
  [[nodiscard]] Result<void> Connect(IInterfaceFactory& factory) override;
  void Disconnect() override;
  [[nodiscard]] Result<void> Init() override;
  void Shutdown() override;
  IInterface* QueryInterface(std::string_view name, u32 version) override;
  void SetControls(IInterfaceFactory& controls) override;

 private:
  Result<void> RegisterSettings();
  void RegisterCounters();

  KernelSettings kernel_settings_;
  IInterfaceFactory* factory_ = nullptr;
  ISettingsRegistry* settings_ = nullptr;
  ILogger* logger_ = nullptr;
  bool hooks_ = false;
  bool forwards_ = false;
  bool xbdm_forwards_ = false;
};

}  // namespace rex::kernel
