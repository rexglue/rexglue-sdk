/**
 * @file        tests/unit/runtime/host_service_modules_test.cpp
 * @brief       The runtime resolves host service backends from modules or its null providers
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <filesystem>

#include <rex/api.h>
#include <rex/apu.h>
#include <rex/gpu.h>
#include <rex/hid.h>
#include <rex/runtime.h>

#include "runtime/apu.h"
#include "runtime/runtime_module.h"
#include "test_memory.h"

namespace {

rex::Config BaseConfig() {
  rex::Config config;
  config.paths.config_file.clear();
  return config;
}

struct Fixture {
  Fixture() {
    rex::testing::ReleaseTestMemory();
    rex::Shutdown();
  }
  ~Fixture() { rex::Shutdown(); }
};

rex::RuntimeDesc SetupConfig(rex::API& api) {
  rex::RuntimeDesc config;
  config.file_system = api.Get<rex::filesystem::IFileSystem>();
  return config;
}

}  // namespace

REX_TEST_CASE_METHOD(Fixture, "Without an apu module the runtime plays to its null backend",
                     "[modules][apu]") {
  rex::Config config = BaseConfig();
  config.modules = {"rex-filesystem", "rex-runtime"};
  auto api = rex::Init(config);
  REQUIRE(api);
  CHECK((*api)->Get<rex::apu::IAPUBackend>() == nullptr);
  CHECK(rex::runtime::RuntimeModule::apu_backend() == nullptr);
  rex::Runtime runtime((*api)->config());
  REQUIRE(runtime.Setup(SetupConfig(**api)));
  CHECK(runtime.apu() != nullptr);
  runtime.Shutdown();
}

REX_TEST_CASE_METHOD(Fixture, "A loaded apu module serves the runtime's backend",
                     "[modules][apu]") {
  rex::Config config = BaseConfig();
  config.modules = {"rex-filesystem", "rex-runtime", "rexmod-test-apu"};
  auto api = rex::Init(config);
  REQUIRE(api);
  auto* served = (*api)->Get<rex::apu::IAPUBackend>();
  REQUIRE(served != nullptr);
  rex::Runtime runtime((*api)->config());
  REQUIRE(runtime.Setup(SetupConfig(**api)));
  CHECK(&runtime.apu()->backend() == served);
  runtime.Shutdown();
}

REX_TEST_CASE_METHOD(Fixture, "Tool mode builds no APU", "[modules][apu]") {
  rex::Config config = BaseConfig();
  config.modules = {"rex-filesystem", "rex-runtime"};
  auto api = rex::Init(config);
  REQUIRE(api);
  rex::Runtime runtime((*api)->config());
  rex::RuntimeDesc setup = SetupConfig(**api);
  setup.tool_mode = true;
  REQUIRE(runtime.Setup(setup));
  CHECK(runtime.apu() == nullptr);
  runtime.SetAudioPaused(true);
  CHECK_FALSE(runtime.audio_paused());
  runtime.Shutdown();
}

REX_TEST_CASE_METHOD(Fixture, "Init rejects apu.max_queued_frames outside 4 to 64",
                     "[modules][apu]") {
  for (rex::u32 value : {3u, 65u}) {
    rex::Config config = BaseConfig();
    config.modules = {"rex-filesystem", "rex-runtime"};
    config.apu.max_queued_frames = value;
    auto api = rex::Init(config);
    CHECK_FALSE(api);
    rex::Shutdown();
  }
}

REX_TEST_CASE_METHOD(Fixture, "Without a hid module the title sees the idle pad",
                     "[modules][hid]") {
  rex::Config config = BaseConfig();
  config.modules = {"rex-filesystem", "rex-runtime"};
  auto api = rex::Init(config);
  REQUIRE(api);
  CHECK((*api)->Get<rex::hid::IHIDBackend>() == nullptr);
  rex::Runtime runtime((*api)->config());
  REQUIRE(runtime.Setup(SetupConfig(**api)));
  CHECK(runtime.hid().GetState(0));
  CHECK_FALSE(runtime.hid().GetState(1));
  runtime.Shutdown();
}

REX_TEST_CASE_METHOD(Fixture, "A loaded hid module serves the runtime's backend",
                     "[modules][hid]") {
  rex::Config config = BaseConfig();
  config.modules = {"rex-filesystem", "rex-runtime", "rexmod-test-hid"};
  auto api = rex::Init(config);
  REQUIRE(api);
  auto* served = (*api)->Get<rex::hid::IHIDBackend>();
  REQUIRE(served != nullptr);
  rex::Runtime runtime((*api)->config());
  REQUIRE(runtime.Setup(SetupConfig(**api)));
  CHECK(&runtime.hid() == served);
  runtime.Shutdown();
}

REX_TEST_CASE_METHOD(Fixture, "Tool mode keeps the hid fallback", "[modules][hid]") {
  rex::Config config = BaseConfig();
  config.modules = {"rex-filesystem", "rex-runtime"};
  auto api = rex::Init(config);
  REQUIRE(api);
  rex::Runtime runtime((*api)->config());
  rex::RuntimeDesc setup = SetupConfig(**api);
  setup.tool_mode = true;
  REQUIRE(runtime.Setup(setup));
  CHECK(runtime.hid().GetState(0));
  runtime.Shutdown();
}

REX_TEST_CASE_METHOD(Fixture, "Without a GPU module the runtime runs on the null GPU",
                     "[modules][gpu]") {
  rex::Config config = BaseConfig();
  config.modules = {"rex-filesystem", "rex-runtime"};
  auto api = rex::Init(config);
  REQUIRE(api);
  CHECK((*api)->Get<rex::gpu::IGPUBackend>() == nullptr);
  rex::Runtime runtime((*api)->config());
  REQUIRE(runtime.Setup(SetupConfig(**api)));
  REQUIRE(runtime.gpu() != nullptr);
  CHECK_FALSE(runtime.gpu()->has_presentation());
  runtime.Shutdown();
}

REX_TEST_CASE_METHOD(Fixture, "Tool mode builds no GPU", "[modules][gpu]") {
  rex::Config config = BaseConfig();
  config.modules = {"rex-filesystem", "rex-runtime"};
  auto api = rex::Init(config);
  REQUIRE(api);
  rex::Runtime runtime((*api)->config());
  rex::RuntimeDesc setup = SetupConfig(**api);
  setup.tool_mode = true;
  REQUIRE(runtime.Setup(setup));
  CHECK(runtime.gpu() == nullptr);
  runtime.Shutdown();
}
