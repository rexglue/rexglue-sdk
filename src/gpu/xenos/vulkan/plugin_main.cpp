/**
 * @file        gpu/xenos/vulkan/plugin_main.cpp
 * @brief       rexgpu-xenos-vulkan module entry, hands the Vulkan graphics system to rexgpu-xenos
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

#include "gpu/xenos/vulkan/cvar_table.h"
#include "gpu/xenos/vulkan/graphics_system.h"
#include "gpu/xenos/cvar_table_pair.h"
#include "gpu/xenos/xenos_graphics.h"

namespace rex::gpu {

class XenosVulkanModule final : public rex::IModule {
 public:
  static void* GetModule();

  static constexpr std::array<rex::InterfaceEntry, 1> kInterfaces = {{
      {rex::IModule::kInterfaceName, rex::IModule::kInterfaceVersion, &GetModule},
  }};

  const char* GetName() const override { return "rexgpu-xenos-vulkan"; }
  std::span<const char* const> GetRequiredInterfaces() const override {
    static constexpr std::array<const char*, 2> kRequired = {"rex.cvar@1", "rex.gpu.xenos@1"};
    return kRequired;
  }

  [[nodiscard]] rex::Result<void> Connect(rex::IInterfaceFactory* factory,
                                          const rex::Config& config) override {
    REX_TRY(gpu::IXenosGraphics * xenos_service, factory->Get<gpu::IXenosGraphics>());
    REX_TRY_VOID(xenos_service->AttachAPI(GetName()));
    attached_ = true;
    xenos_ = xenos_service;
    REX_TRY(HostAPI host_api, SelectHostAPI(config.gpu.backend, kPlatformHostAPIs));
    if (host_api != HostAPI::kVulkan) {
      return rex::Err(rex::ErrorCategory::kConfig,
                      std::format("rexgpu-xenos-vulkan was loaded but gpu.backend is '{}'",
                                  config.gpu.backend));
    }
    REX_TRY(rex::cvar::ICvarRegistry * registry, factory->Get<rex::cvar::ICvarRegistry>());
    REX_TRY_VOID(registry->Register("vulkan", cvars::kVulkanCvars));
    cvars_ = registry;
    REX_TRY_VOID(cvars_->Add(cvars::kPairCvars));
    REX_TRY_VOID(cvars_->Add(cvars::kVulkanXenosCvars));
    return rex::Ok();
  }

  void Disconnect() override {
    if (cvars_) {
      cvars_->Unregister(cvars::kVulkanXenosCvars);
      cvars_->Unregister(cvars::kPairCvars);
      cvars_->Unregister(cvars::kVulkanCvars);
    }
    if (xenos_ && attached_) {
      xenos_->DetachAPI();
    }
    attached_ = false;
    xenos_ = nullptr;
    cvars_ = nullptr;
  }

  [[nodiscard]] rex::Result<void> Init() override {
    return xenos_->SetBackend(std::make_unique<vulkan::VulkanGraphicsSystem>());
  }

  void Shutdown() override {}

 private:
  static XenosVulkanModule& GetInstance() {
    static XenosVulkanModule instance;
    return instance;
  }

  gpu::IXenosGraphics* xenos_ = nullptr;
  rex::cvar::ICvarRegistry* cvars_ = nullptr;
  bool attached_ = false;
};

void* XenosVulkanModule::GetModule() {
  return static_cast<rex::IModule*>(&GetInstance());
}

}  // namespace rex::gpu

REX_DECLARE_MODULE(rex::gpu::XenosVulkanModule)
