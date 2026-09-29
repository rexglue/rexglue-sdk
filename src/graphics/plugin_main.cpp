/**
 * @file        graphics/plugin_main.cpp
 * @brief       rexgpu-xenos module entry, serves rex.module@1 and rex.graphics.backend@1
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include <rex/api.h>
#include <rex/cvar.h>
#include <rex/graphics/graphics_system.h>
#include <rex/logging.h>
#include <rex/module.h>

#include "graphics/cvar_table.h"

#if REX_HAS_D3D12
#include <rex/graphics/d3d12/graphics_system.h>
#endif
#if REX_HAS_VULKAN
#include <rex/graphics/vulkan/graphics_system.h>
#endif

namespace rex::graphics::xenos {

class XenosModule final : public rex::IModule {
 public:
  static void* GetModule();
  static void* GetBackend();

  static constexpr std::array<rex::InterfaceEntry, 2> kInterfaces = {{
      {rex::IModule::kInterfaceName, rex::IModule::kInterfaceVersion, &GetModule},
      {IGraphicsBackend::kInterfaceName, IGraphicsBackend::kInterfaceVersion, &GetBackend},
  }};

  const char* Name() const override { return "rexgpu-xenos"; }
  std::span<const char* const> RequiredInterfaces() const override {
    static constexpr std::array<const char*, 1> kRequired = {"rex.cvar@1"};
    return kRequired;
  }

  rex::Status Connect(rex::IInterfaceFactory* factory, const rex::Config& config) override {
    config_ = &config.graphics;
    cvars_ = factory->Get<rex::cvar::ICvarRegistry>();
    if (!cvars_) {
      return rex::Err(rex::ErrorCategory::Module, "rexgpu-xenos: rex.cvar is not served");
    }
    REX_TRY_VOID(cvars_->Register("gpu", cvars::kGPUCvars));
    REX_TRY_VOID(
        cvars_->SetDefault("gpu.resolution_scale_x", std::to_string(config_->resolution_scale_x)));
    REX_TRY_VOID(
        cvars_->SetDefault("gpu.resolution_scale_y", std::to_string(config_->resolution_scale_y)));
    return rex::Ok();
  }

  void Disconnect() override {
    if (cvars_)
      cvars_->Unregister(cvars::kGPUCvars);
    cvars_ = nullptr;
    config_ = nullptr;
  }

  rex::Status Init() override {
    std::string_view backend = config_ ? std::string_view(config_->backend) : "any";
#if REX_HAS_D3D12
    if (backend == "any" || backend == "d3d12") {
      backend_ = std::make_unique<d3d12::D3D12GraphicsSystem>();
      backend_->set_config(config_);
      return rex::Ok();
    }
#endif
#if REX_HAS_VULKAN
    if (backend == "any" || backend == "vulkan") {
      backend_ = std::make_unique<vulkan::VulkanGraphicsSystem>();
      backend_->set_config(config_);
      return rex::Ok();
    }
#endif
    return rex::Err(
        rex::ErrorCategory::Module,
        fmt::format("rexgpu-xenos: backend '{}' is not compiled into this module", backend));
  }

  void Shutdown() override { backend_.reset(); }

 private:
  static XenosModule& Instance() {
    static XenosModule instance;
    return instance;
  }

  const BackendConfig* config_ = nullptr;
  rex::cvar::ICvarRegistry* cvars_ = nullptr;
  std::unique_ptr<GraphicsSystem> backend_;
};

void* XenosModule::GetModule() {
  return static_cast<rex::IModule*>(&Instance());
}

void* XenosModule::GetBackend() {
  return static_cast<IGraphicsBackend*>(Instance().backend_.get());
}

}  // namespace rex::graphics::xenos

REX_DECLARE_MODULE(rex::graphics::xenos::XenosModule)
