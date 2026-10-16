/**
 * @file        gpu/xenos/plugin_main.cpp
 * @brief       rexgpu-xenos module entry, serves the module and graphics backend interfaces
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
#include <rex/logging/logger.h>
#include <rex/logging/log.h>
#include <rex/module.h>
#include <rex/platform.h>

#include "gpu/xenos/cvar_table_gpu.h"
#include "gpu/xenos/cvar_table_xenos.h"
#include "gpu/xenos/xenos_graphics.h"

REXLOG_TABLE(kGPULog, gpu)

#include "gpu/xenos/null/null_graphics_system.h"

#include <rex/gpu/backend_select.h>

namespace rex::gpu {

class XenosModule final : public rex::IModule, public IXenosGraphics {
 public:
  static void* GetModule();
  static void* GetXenos();
  static void* GetBackend();

  static constexpr std::array<rex::InterfaceEntry, 3> kInterfaces = {{
      {rex::IModule::kInterfaceName, rex::IModule::kInterfaceVersion, &GetModule},
      {gpu::IXenosGraphics::kInterfaceName, gpu::IXenosGraphics::kInterfaceVersion, &GetXenos},
      {gpu::IGPUBackend::kInterfaceName, gpu::IGPUBackend::kInterfaceVersion, &GetBackend},
  }};

  const char* GetName() const override { return "rexgpu-xenos"; }
  std::span<const char* const> GetRequiredInterfaces() const override {
    static constexpr std::array<const char*, 1> kRequired = {"rex.cvar@1"};
    return kRequired;
  }

  [[nodiscard]] rex::Result<void> Connect(rex::IInterfaceFactory* factory,
                                          const rex::Config& config) override {
    const gpu::GPUConfig& gpu = config.gpu;
    if (gpu.resolution_scale_x < 1 || gpu.resolution_scale_x > 8) {
      return rex::Err(rex::ErrorCategory::kConfig,
                      std::format("config field gpu.resolution_scale_x = {} is outside 1 to 8",
                                  gpu.resolution_scale_x));
    }
    if (gpu.resolution_scale_y < 1 || gpu.resolution_scale_y > 8) {
      return rex::Err(rex::ErrorCategory::kConfig,
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
    REX_TRY_VOID(
        cvars_->SetDefault("gpu.resolution_scale_x", std::to_string(config_->resolution_scale_x)));
    REX_TRY_VOID(
        cvars_->SetDefault("gpu.resolution_scale_y", std::to_string(config_->resolution_scale_y)));
    return rex::Ok();
  }

  void Disconnect() override {
    if (cvars_) {
      cvars_->Unregister(cvars::kXenosCvars);
      cvars_->Unregister(cvars::kGPUCvars);
    }
    if (logger_)
      logger_->Unregister(rex::log::kGPULog);
    logger_ = nullptr;
    cvars_ = nullptr;
    config_ = nullptr;
  }

  [[nodiscard]] rex::Result<void> Init() override {
    std::string_view name = config_ ? std::string_view(config_->backend) : "any";
    REX_TRY(HostAPI api, SelectHostAPI(name, kPlatformHostAPIs));
    if (api == HostAPI::kNull) {
      return SetBackend(std::make_unique<null::NullGraphicsSystem>());
    }
    if (api_module_.empty()) {
      return rex::Err(
          rex::ErrorCategory::kConfig,
          std::format("gpu.backend '{}' needs {} in the module list", name,
                      api == HostAPI::kD3D12 ? "rexgpu-xenos-d3d12" : "rexgpu-xenos-vulkan"));
    }
    return rex::Ok();
  }

  void Shutdown() override { backend_.reset(); }

  const gpu::GPUConfig* GetConfig() const override { return config_; }

  [[nodiscard]] rex::Result<void> AttachAPI(std::string_view module) override {
    if (!api_module_.empty()) {
      return rex::Err(rex::ErrorCategory::kConfig,
                      std::format("{} and {} cannot both be loaded", api_module_, module));
    }
    api_module_ = module;
    return rex::Ok();
  }

  void DetachAPI() override {
    backend_.reset();
    api_module_.clear();
  }

  [[nodiscard]] rex::Result<void> SetBackend(std::unique_ptr<GraphicsSystem> backend) override {
    if (backend && backend_) {
      return rex::Err(rex::ErrorCategory::kConfig, "a graphics system is already set");
    }
    backend_ = std::move(backend);
    if (backend_) {
      backend_->SetConfig(config_);
    }
    return rex::Ok();
  }

 private:
  static XenosModule& GetInstance() {
    static XenosModule instance;
    return instance;
  }

  const gpu::GPUConfig* config_ = nullptr;
  rex::cvar::ICvarRegistry* cvars_ = nullptr;
  rex::ILogger* logger_ = nullptr;
  std::unique_ptr<GraphicsSystem> backend_;
  std::string api_module_;
};

void* XenosModule::GetModule() {
  return static_cast<rex::IModule*>(&GetInstance());
}

void* XenosModule::GetXenos() {
  return static_cast<gpu::IXenosGraphics*>(&GetInstance());
}

void* XenosModule::GetBackend() {
  return static_cast<gpu::IGPUBackend*>(GetInstance().backend_.get());
}

}  // namespace rex::gpu

REX_DECLARE_MODULE(rex::gpu::XenosModule)
