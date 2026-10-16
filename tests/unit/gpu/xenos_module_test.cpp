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

#include <string>

#include <rex/api.h>
#include <rex/gpu.h>

#include "gpu/xenos/graphics_system.h"
#include "gpu/xenos/xenos_graphics.h"

namespace {

rex::Config BaseConfig() {
  rex::Config config;
  config.paths.config_file.clear();
  config.modules = {"rexfilesystem", "rexruntime", "rexgpu-xenos"};
  return config;
}

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
