/**
 * @file        graphics/plugin_main.cpp
 * @brief       rex-gpu-xenos module entry, serves the module and graphics backend interfaces
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
#include <string>
#include <string_view>

#include <rex/api.h>
#include <rex/cvar.h>
#include "gpu/xenos/graphics_system.h"
#include <rex/logging.h>
#include <rex/module.h>
#include <rex/platform.h>

#include "gpu/xenos/cvar_table.h"

REXLOG_TABLE(kGPULog, gpu)

#if REX_PLATFORM_WIN32
#include "gpu/xenos/d3d12/graphics_system.h"
#endif
#include "gpu/xenos/vulkan/graphics_system.h"
#include "gpu/xenos/null/null_graphics_system.h"

#include "gpu/xenos/backend_select.h"

namespace rex::gpu {

class XenosModule final : public rex::IModule {
 public:
  static void* GetModule();
  static void* GetBackend();

  static constexpr std::array<rex::InterfaceEntry, 2> kInterfaces = {{
      {rex::IModule::kInterfaceName, rex::IModule::kInterfaceVersion, &GetModule},
      {gpu::IBackend::kInterfaceName, gpu::IBackend::kInterfaceVersion, &GetBackend},
  }};

  const char* Name() const override { return "rex-gpu-xenos"; }
  std::span<const char* const> RequiredInterfaces() const override {
    static constexpr std::array<const char*, 1> kRequired = {"rex.cvar@1"};
    return kRequired;
  }

  rex::Result<void> Connect(rex::IInterfaceFactory* factory, const rex::Config& config) override {
    const gpu::GPUConfig& gpu = config.gpu;
    if (gpu.resolution_scale_x < 1 || gpu.resolution_scale_x > 8) {
      return rex::Err(rex::ErrorCategory::Config,
                      std::format("config field gpu.resolution_scale_x = {} is outside 1 to 8",
                                  gpu.resolution_scale_x));
    }
    if (gpu.resolution_scale_y < 1 || gpu.resolution_scale_y > 8) {
      return rex::Err(rex::ErrorCategory::Config,
                      std::format("config field gpu.resolution_scale_y = {} is outside 1 to 8",
                                  gpu.resolution_scale_y));
    }
    REX_TRY_VOID(SelectHostAPI(gpu.backend, kPlatformHostAPIs));
    config_ = &gpu;
    REX_TRY(cvars_, factory->Get<rex::cvar::ICvarRegistry>());
    REX_TRY(logger_, factory->Get<rex::ILogger>());
    REX_TRY_VOID(logger_->Register(rex::log::kGPULog));
    REX_TRY_VOID(cvars_->Register("gpu", cvars::kGPUCvars));
    REX_TRY_VOID(cvars_->Register("xenos", cvars::kXenosCvars));
    REX_TRY_VOID(cvars_->Register("d3d12", cvars::kD3D12Cvars));
    REX_TRY_VOID(cvars_->Register("vulkan", cvars::kVulkanCvars));
    REX_TRY_VOID(
        cvars_->SetDefault("gpu.resolution_scale_x", std::to_string(config_->resolution_scale_x)));
    REX_TRY_VOID(
        cvars_->SetDefault("gpu.resolution_scale_y", std::to_string(config_->resolution_scale_y)));
    return rex::Ok();
  }

  void Disconnect() override {
    if (cvars_) {
      cvars_->Unregister(cvars::kVulkanCvars);
      cvars_->Unregister(cvars::kD3D12Cvars);
      cvars_->Unregister(cvars::kXenosCvars);
      cvars_->Unregister(cvars::kGPUCvars);
    }
    if (logger_)
      logger_->Unregister(rex::log::kGPULog);
    logger_ = nullptr;
    cvars_ = nullptr;
    config_ = nullptr;
  }

  rex::Result<void> Init() override {
    std::string_view name = config_ ? std::string_view(config_->backend) : "any";
    REX_TRY(HostAPI api, SelectHostAPI(name, kPlatformHostAPIs));
    switch (api) {
#if REX_PLATFORM_WIN32
      case HostAPI::kD3D12:
        backend_ = std::make_unique<d3d12::D3D12GraphicsSystem>();
        break;
#endif
      case HostAPI::kNull:
        backend_ = std::make_unique<null::NullGraphicsSystem>();
        break;
      default:
        backend_ = std::make_unique<vulkan::VulkanGraphicsSystem>();
        break;
    }
    backend_->set_config(config_);
    return rex::Ok();
  }

  void Shutdown() override { backend_.reset(); }

 private:
  static XenosModule& Instance() {
    static XenosModule instance;
    return instance;
  }

  const gpu::GPUConfig* config_ = nullptr;
  rex::cvar::ICvarRegistry* cvars_ = nullptr;
  rex::ILogger* logger_ = nullptr;
  std::unique_ptr<GraphicsSystem> backend_;
};

void* XenosModule::GetModule() {
  return static_cast<rex::IModule*>(&Instance());
}

void* XenosModule::GetBackend() {
  return static_cast<gpu::IBackend*>(Instance().backend_.get());
}

}  // namespace rex::gpu

REX_DECLARE_MODULE(rex::gpu::XenosModule)
