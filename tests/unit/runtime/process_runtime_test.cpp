/**
 * @file        tests/unit/runtime/process_runtime_test.cpp
 * @brief       The process wide runtime accessor
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <filesystem>
#include <thread>

#include <rex/api.h>
#include <rex/gpu.h>
#include <rex/runtime.h>

#include "../kernel/kernel_fixture.h"
#include "base_config.h"
#include "test_memory.h"

REX_TEST_CASE("GetProcessRuntime is null before Setup and after Shutdown", "[runtime]") {
  CHECK(rex::GetProcessRuntime() == nullptr);
  {
    rex::test::KernelFixture fixture;
    CHECK(rex::GetProcessRuntime() == &fixture.GetRuntime());
  }
  CHECK(rex::GetProcessRuntime() == nullptr);
}

REX_TEST_CASE("GetProcessRuntime works on a thread with no runtime", "[runtime]") {
  rex::test::KernelFixture fixture;
  rex::Runtime* seen = nullptr;
  std::thread host([&seen] { seen = rex::GetProcessRuntime(); });
  host.join();
  CHECK(seen == &fixture.GetRuntime());
}

namespace {

class ShutdownWatchGPU final : public rex::gpu::IGPUBackend {
 public:
  rex::Runtime* seen = reinterpret_cast<rex::Runtime*>(1);

  [[nodiscard]] rex::Result<void> SetupPresentation(rex::gpu::IUIThread&) override {
    return rex::Ok();
  }
  [[nodiscard]] rex::Result<void> SetupGPU(rex::runtime::FunctionDispatcher&,
                                           rex::Runtime&) override {
    return rex::Ok();
  }
  bool HasPresentation() const override { return false; }
  rex::gpu::IPresenter* GetPresenter() const override { return nullptr; }
  [[nodiscard]] rex::Result<std::unique_ptr<rex::gpu::IPresenter>> CreatePresenter() override {
    return rex::Err<std::unique_ptr<rex::gpu::IPresenter>>(rex::ErrorCategory::kRuntime, "none");
  }
  [[nodiscard]] rex::Result<std::unique_ptr<rex::gpu::IOverlayDrawer>> CreateOverlayDrawer(
      rex::gpu::IPresenter&) override {
    return rex::Err<std::unique_ptr<rex::gpu::IOverlayDrawer>>(rex::ErrorCategory::kRuntime,
                                                               "none");
  }
  void SetInterruptCallback(rex::u32, rex::u32) override {}
  void InitializeRingBuffer(rex::u32, rex::u32) override {}
  void EnableReadPointerWriteBack(rex::u32, rex::u32) override {}
  [[nodiscard]] rex::Result<void> InitializeShaderStorage(const std::filesystem::path&, rex::u32,
                                                          bool) override {
    return rex::Ok();
  }
  void Shutdown() override { seen = rex::GetProcessRuntime(); }
};

}  // namespace

REX_TEST_CASE("GetProcessRuntime is null while Shutdown tears the subsystems down", "[runtime]") {
  rex::test::ReleaseTestMemory();
  rex::Shutdown();
  rex::Config config = rex::test::BaseConfig();
  config.modules = {"rexfilesystem", "rexruntime"};
  auto api = rex::Init(config);
  REQUIRE(api);
  ShutdownWatchGPU watch;
  rex::Runtime runtime((*api)->GetConfig());
  rex::RuntimeDesc setup;
  setup.file_system = (*api)->Get<rex::filesystem::IFileSystem>();
  setup.gpu = &watch;
  REQUIRE(runtime.Setup(setup));
  CHECK(rex::GetProcessRuntime() == &runtime);
  runtime.Shutdown();
  CHECK(watch.seen == nullptr);
  rex::Shutdown();
}
