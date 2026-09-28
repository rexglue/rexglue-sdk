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
#include <string_view>

#include <fmt/format.h>

#include <rex/api.h>
#include <rex/graphics/graphics_system.h>
#include <rex/logging.h>
#include <rex/module.h>

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
  std::span<const char* const> RequiredInterfaces() const override { return {}; }

  rex::Status Connect(rex::IInterfaceFactory*, const rex::Config& config) override {
    config_ = &config.graphics;
    return rex::Ok();
  }

  void Disconnect() override { config_ = nullptr; }

  rex::Status Init() override {
    std::string_view backend = config_ ? std::string_view(config_->backend) : "any";
#if REX_HAS_D3D12
    if (backend == "any" || backend == "d3d12") {
      backend_ = std::make_unique<d3d12::D3D12GraphicsSystem>();
      return rex::Ok();
    }
#endif
#if REX_HAS_VULKAN
    if (backend == "any" || backend == "vulkan") {
      backend_ = std::make_unique<vulkan::VulkanGraphicsSystem>();
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
