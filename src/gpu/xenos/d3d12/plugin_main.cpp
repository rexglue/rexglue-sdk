/**
 * @file        gpu/xenos/d3d12/plugin_main.cpp
 * @brief       rexgpu-xenos-d3d12 module entry, hands the Direct3D 12 graphics system to
 * rexgpu-xenos
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>
#include <format>
#include <memory>
#include <span>

#include <rex/api.h>
#include <rex/cvar.h>
#include <rex/module.h>
#include <rex/gpu/backend_select.h>

#include "gpu/xenos/d3d12/cvar_table.h"
#include "gpu/xenos/d3d12/graphics_system.h"
#include "gpu/xenos/cvar_table_pair.h"
#include "gpu/xenos/xenos_graphics.h"

namespace rex::gpu {

class XenosD3D12Module final : public rex::IModule {
 public:
  static void* GetModule();

  static constexpr std::array<rex::InterfaceEntry, 1> kInterfaces = {{
      {rex::IModule::kInterfaceName, rex::IModule::kInterfaceVersion, &GetModule},
  }};

  const char* GetName() const override { return "rexgpu-xenos-d3d12"; }
  std::span<const char* const> GetRequiredInterfaces() const override {
    static constexpr std::array<const char*, 2> kRequired = {"rex.cvar@1", "rex.gpu.xenos@1"};
    return kRequired;
  }

  [[nodiscard]] rex::Result<void> Connect(rex::IInterfaceFactory* factory,
                                          const rex::Config& config) override {
    if (auto status = ConnectModule(factory, config); !status) {
      Disconnect();
      return status;
    }
    return rex::Ok();
  }

  [[nodiscard]] rex::Result<void> ConnectModule(rex::IInterfaceFactory* factory,
                                                const rex::Config& config) {
    REX_TRY(gpu::IXenosGraphics * xenos_service, factory->Get<gpu::IXenosGraphics>());
    REX_TRY_VOID(xenos_service->AttachAPI(GetName()));
    attached_ = true;
    xenos_ = xenos_service;
    REX_TRY(HostAPI host_api, SelectHostAPI(config.gpu.backend, kPlatformHostAPIs));
    if (host_api != HostAPI::kD3D12) {
      return rex::Err(
          rex::ErrorCategory::kConfig,
          std::format("rexgpu-xenos-d3d12 was loaded but gpu.backend is '{}'", config.gpu.backend));
    }
    REX_TRY(rex::cvar::ICvarRegistry * registry, factory->Get<rex::cvar::ICvarRegistry>());
    REX_TRY_VOID(registry->Register("d3d12", cvars::kD3D12Cvars));
    cvars_ = registry;
    REX_TRY_VOID(cvars_->Add(cvars::kPairCvars));
    REX_TRY_VOID(cvars_->Add(cvars::kD3D12XenosCvars));
    return rex::Ok();
  }

  void Disconnect() override {
    if (cvars_) {
      cvars_->Unregister(cvars::kD3D12XenosCvars);
      cvars_->Unregister(cvars::kPairCvars);
      cvars_->Unregister(cvars::kD3D12Cvars);
    }
    if (xenos_ && attached_) {
      xenos_->DetachAPI();
    }
    attached_ = false;
    xenos_ = nullptr;
    cvars_ = nullptr;
  }

  [[nodiscard]] rex::Result<void> Init() override {
    return xenos_->SetBackend(std::make_unique<d3d12::D3D12GraphicsSystem>());
  }

  void Shutdown() override {}

 private:
  static XenosD3D12Module& GetInstance() {
    static XenosD3D12Module instance;
    return instance;
  }

  gpu::IXenosGraphics* xenos_ = nullptr;
  rex::cvar::ICvarRegistry* cvars_ = nullptr;
  bool attached_ = false;
};

void* XenosD3D12Module::GetModule() {
  return static_cast<rex::IModule*>(&GetInstance());
}

}  // namespace rex::gpu

REX_DECLARE_MODULE(rex::gpu::XenosD3D12Module)
