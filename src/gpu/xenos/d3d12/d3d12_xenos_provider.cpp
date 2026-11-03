/**
 * @file        gpu/xenos/d3d12/d3d12_xenos_provider.cpp
 * @brief       Provides the Direct3D 12 Xenos graphics driver to the graphics system
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

#include "gpu/xenos/xenos_counters.h"
#include "gpu/xenos/xenos_graphics_driver_base.h"
#include "gpu/xenos/host/d3d12/d3d12_provider.h"
#include "gpu/xenos/d3d12/d3d12_xenos_graphics_driver.h"

namespace rex::gpu::d3d12 {

namespace {

std::unique_ptr<IDriver> CreateDriver() {
  return std::make_unique<D3D12XenosGraphicsDriver>();
}

constexpr std::array<InterfaceType, 1> kRequired = {IGraphicsSystem::kInterface};

constexpr std::array<DriverRegistration, 1> kDrivers = {
    DriverRegistration{DriverType::kGraphics, "d3d12", GraphicsFamily::kXenos, &CreateDriver}};

}  // namespace

class D3D12XenosProvider final : public IModule, public IDriverModule {
 public:
  REX_MODULE_SERVES(::rex::IDriverModule)

  std::string_view GetName() const override { return "rexgpu-xenos-d3d12"; }
  std::span<const InterfaceType> GetRequiredInterfaces() const override { return kRequired; }
  [[nodiscard]] Result<void> Connect(IFactory& factory) override {
    rex::memory::SetTagName(rex::gpu::kGPU, "GPU");
    rex::gpu::RegisterXenosCounters();
    if (auto* graphics = factory.Get<IGraphicsSystem>()) {
      XenosGraphicsDriverBase::SetGraphicsSettings(&graphics->GetSettings());
    }
    if (!rex::gpu::d3d12::D3D12Provider::IsD3D12APIAvailable()) {
      return Err(ErrorCategory::kRuntime, "Direct3D 12 is not available");
    }
    return {};
  }
  void Disconnect() override { XenosGraphicsDriverBase::SetGraphicsSettings(nullptr); }
  [[nodiscard]] Result<void> Init() override { return {}; }
  void Shutdown() override {}
  IInterface* QueryInterface(std::string_view name, u32 version) override {
    return FindModuleInterface<IDriverModule>(*this, name, version);
  }
  std::span<const DriverRegistration> GetDrivers() const override { return kDrivers; }
};

}  // namespace rex::gpu::d3d12

REX_DECLARE_MODULE(rex::gpu::d3d12::D3D12XenosProvider)
