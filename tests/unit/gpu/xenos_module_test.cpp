/**
 * @file        tests/unit/gpu/xenos_module_test.cpp
 * @brief       The neutral Xenos module owns the host API slot and the graphics system it serves
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <algorithm>
#include <string>

#include <rex/api.h>
#include <rex/cvar/registry.h>
#include <rex/gpu.h>
#include <rex/module.h>
#include <rex/platform/dynlib.h>
#include <rex/platform/file.h>

#include "core/cvar_registry.h"
#include "core/module_loader.h"
#include "gpu/xenos/cvar_table_pair.h"
#include "gpu/xenos/graphics_system.h"
#include "gpu/xenos/xenos_graphics.h"

namespace {

rex::Config BaseConfig() {
  rex::Config config;
  config.paths.config_file.clear();
  config.modules = {"rexfilesystem", "rexruntime", "rexgpu-xenos"};
  return config;
}

class SlotXenos final : public rex::gpu::IXenosGraphics {
 public:
  const rex::gpu::GPUConfig* GetConfig() const override { return nullptr; }
  [[nodiscard]] rex::Result<void> AttachAPI(std::string_view module) override {
    attached = std::string(module);
    return rex::Ok();
  }
  void DetachAPI() override { attached.clear(); }
  [[nodiscard]] rex::Result<void> SetBackend(std::unique_ptr<rex::gpu::GraphicsSystem>) override {
    return rex::Ok();
  }

  std::string attached;
};

class SlotFactory final : public rex::IInterfaceFactory {
 public:
  [[nodiscard]] rex::Result<void*> Get(std::string_view name, rex::u32) override {
    if (name == rex::cvar::ICvarRegistry::kInterfaceName) {
      return static_cast<void*>(registry);
    }
    if (name == rex::gpu::IXenosGraphics::kInterfaceName) {
      return static_cast<void*>(static_cast<rex::gpu::IXenosGraphics*>(xenos));
    }
    return rex::Err<void*>(rex::ErrorCategory::kModule, "not served");
  }

  rex::cvar::ICvarRegistry* registry = nullptr;
  SlotXenos* xenos = nullptr;
};

struct Fixture {
  Fixture() { rex::Shutdown(); }
  ~Fixture() { rex::Shutdown(); }
};

}  // namespace

REX_TEST_CASE_METHOD(Fixture, "A host API backend without its library fails Init naming it",
                     "[gpu][xenos]") {
  rex::Config config = BaseConfig();
  config.gpu.backend = "vulkan";
  auto api = rex::Init(config);
  REQUIRE_FALSE(api);
  CHECK(api.error().message.find("rexgpu-xenos-vulkan") != std::string::npos);
}

REX_TEST_CASE_METHOD(Fixture, "Detaching the host API destroys the graphics system it handed over",
                     "[gpu][xenos]") {
  rex::Config config = BaseConfig();
  config.gpu.backend = "null";
  auto api = rex::Init(config);
  REQUIRE(api);
  auto* xenos = api.value()->Get<rex::gpu::IXenosGraphics>();
  REQUIRE(xenos);
  REQUIRE(api.value()->Get<rex::gpu::IGPUBackend>());
  REQUIRE(xenos->AttachAPI("rexgpu-xenos-test"));
  xenos->DetachAPI();
  CHECK_FALSE(api.value()->Get<rex::gpu::IGPUBackend>());
}

REX_TEST_CASE_METHOD(Fixture, "Clearing the graphics system is always accepted", "[gpu][xenos]") {
  rex::Config config = BaseConfig();
  config.gpu.backend = "null";
  auto api = rex::Init(config);
  REQUIRE(api);
  auto* xenos = api.value()->Get<rex::gpu::IXenosGraphics>();
  REQUIRE(xenos);
  CHECK(xenos->SetBackend(nullptr));
  CHECK(xenos->SetBackend(nullptr));
  CHECK_FALSE(api.value()->Get<rex::gpu::IGPUBackend>());
}

REX_TEST_CASE("A host API library that fails Connect releases its cvars and the API slot",
              "[gpu][xenos]") {
  rex::platform::DynamicLibrary library;
  REQUIRE(library.Load(
      rex::platform::GetExecutableFolder() / rex::detail::ModuleFileName("rexgpu-xenos-vulkan"),
      rex::platform::SymbolResolution::kImmediate));
  auto create = library.GetSymbol<rex::CreateInterfaceFn>(rex::kCreateInterfaceSymbol);
  REQUIRE(create != nullptr);
  rex::i32 status = 0;
  auto* module = static_cast<rex::IModule*>(create("rex.module", 1, &status));
  REQUIRE(module != nullptr);

  auto registry = rex::detail::CreateCvarRegistry();
  REQUIRE(registry->Register("gpu", {}));
  REQUIRE(registry->Register("xenos", {}));
  REQUIRE(registry->Add(rex::gpu::cvars::kPairCvars));
  SlotXenos xenos;
  SlotFactory factory;
  factory.registry = registry.get();
  factory.xenos = &xenos;
  rex::Config config;
  config.gpu.backend = "vulkan";

  REQUIRE_FALSE(module->Connect(&factory, config));
  CHECK(xenos.attached.empty());
  CHECK_FALSE(registry->Get("vulkan.log_debug_messages"));
  CHECK_FALSE(std::ranges::contains(registry->GetScopes(), "vulkan"));
}
