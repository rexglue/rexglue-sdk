/**
 * @file        gpu/xenos/metal/metal_xenos_provider.cpp
 * @brief       Module providing the Metal Xenos driver
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>
#include <memory>

#include <rex/driver.h>
#include <rex/driver_registration.h>
#include <rex/driver_module.h>
#include <rex/gpu/graphics_driver.h>
#include <rex/gpu/graphics_system.h>
#include <rex/ui/metal/metal_provider.h>

#include "gpu/xenos/metal/metal_xenos_graphics_driver.h"
#include "gpu/xenos/xenos_counters.h"
#include "gpu/xenos/xenos_graphics_driver_base.h"

namespace rex::gpu::metal {

namespace {

std::unique_ptr<IDriver> CreateDriver() {
  return std::make_unique<MetalXenosGraphicsDriver>();
}

constexpr std::array<InterfaceType, 1> kRequired = {IGraphicsSystem::kInterface};

constexpr std::array<DriverRegistration, 1> kDrivers = {
    DriverRegistration{DriverType::kGraphics, "metal", GraphicsFamily::kXenos, &CreateDriver}};

}  // namespace

class MetalXenosProvider final : public IModule, public IDriverModule {
 public:
  REX_MODULE_SERVES(::rex::IDriverModule)

  std::string_view GetName() const override { return "rexgpu-xenos-metal"; }
  std::span<const InterfaceType> GetRequiredInterfaces() const override { return kRequired; }
  [[nodiscard]] Result<void> Connect(IInterfaceFactory& factory) override {
    rex::memory::SetTagName(rex::gpu::kGPU, "GPU");
    rex::gpu::RegisterXenosCounters();
    // The device and queue live in the UI layer's provider, and this module is what brings the
    // two together: the driver asks for a provider through this factory instead of naming it.
    MetalProviderFactory = []() -> std::unique_ptr<::rex::gpu::GraphicsProvider> {
      return ::rex::ui::metal::MetalProvider::Create();
    };
    if (auto* graphics = factory.Get<IGraphicsSystem>()) {
      XenosGraphicsDriverBase::SetGraphicsSettings(&graphics->GetSettings());
    }
    return {};
  }
  void Disconnect() override {
    MetalProviderFactory = nullptr;
    XenosGraphicsDriverBase::SetGraphicsSettings(nullptr);
  }
  [[nodiscard]] Result<void> Init() override { return {}; }
  void Shutdown() override {}
  IInterface* QueryInterface(std::string_view name, u32 version) override {
    return FindModuleInterface<IDriverModule>(*this, name, version);
  }
  std::span<const DriverRegistration> GetDrivers() const override { return kDrivers; }
};

}  // namespace rex::gpu::metal

REX_DECLARE_MODULE(rex::gpu::metal::MetalXenosProvider)
