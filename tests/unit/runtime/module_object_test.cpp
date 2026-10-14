/**
 * @file        tests/unit/runtime/module_object_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <memory>
#include <string>

#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <rex/runtime/module.h>
#include <rex/runtime/module_object.h>
#include <rex/runtime/object_table.h>
#include <rex/runtime/xex_module.h>
#include <rex/runtime/xmemory.h>

#include "core/xex_builder.h"
#include "kernel/kernel_fixture.h"
#include "runtime/module_registry.h"

namespace {

using rex::runtime::ModuleFormat;
using rex::runtime::ModuleObject;
using rex::runtime::ModuleType;

class CountingModule : public rex::runtime::Module {
 public:
  CountingModule(rex::runtime::FunctionDispatcher* dispatcher, u32* destroyed)
      : Module(dispatcher), destroyed_(destroyed) {}
  ~CountingModule() override { ++*destroyed_; }
  const std::string& GetName() const override { return name_; }
  bool IsExecutable() const override { return false; }

 private:
  std::string name_ = "counting";
  u32* destroyed_;
};

class WatchedXEXModule : public rex::runtime::XEXModule {
 public:
  WatchedXEXModule(rex::Runtime& runtime, u32* destroyed, bool* loaded_at_destroy)
      : XEXModule(runtime.GetFunctionDispatcher(), runtime),
        destroyed_(destroyed),
        loaded_at_destroy_(loaded_at_destroy) {}
  ~WatchedXEXModule() override {
    ++*destroyed_;
    *loaded_at_destroy_ = GetLoaded();
  }

 private:
  u32* destroyed_;
  bool* loaded_at_destroy_;
};

}  // namespace

REX_TEST_CASE("A user module object frees its loaded module when it is destroyed",
              "[runtime][module]") {
  rex::testing::KernelFixture fixture;
  rex::Runtime& runtime = fixture.GetRuntime();
  u32 destroyed = 0;
  const rex::X_HANDLE handle = rex::runtime::CreateModule(runtime, ModuleType::kUser);
  REQUIRE(handle != 0);
  {
    auto module = runtime.GetObjectTable().Get<ModuleObject>(handle);
    REQUIRE(module);
    module->user.module_format = ModuleFormat::kELF;
    module->processor_module =
        std::make_unique<CountingModule>(runtime.GetFunctionDispatcher(), &destroyed);
  }
  CHECK(destroyed == 0);
  rex::runtime::CloseOwned(runtime.GetObjectTable(), handle);
  CHECK(destroyed == 1);
}

REX_TEST_CASE("A user module object unloads its XEX image before freeing the module",
              "[runtime][module]") {
  rex::testing::KernelFixture fixture;
  rex::Runtime& runtime = fixture.GetRuntime();
  rex::test::XEXSpec spec;
  spec.image = rex::test::TestImage(0x1000);
  const auto file = rex::test::BuildXEX(spec);
  u32 destroyed = 0;
  bool loaded_at_destroy = true;
  auto xex = std::make_unique<WatchedXEXModule>(runtime, &destroyed, &loaded_at_destroy);
  REQUIRE(xex->Load("watched", "game:\\watched.xex", file.data(), file.size()));
  rex::runtime::BaseHeap* heap = runtime.GetMemory()->LookupHeap(spec.load_address);
  rex::runtime::HeapAllocationInfo info{};
  REQUIRE(heap->QueryRegionInfo(spec.load_address, &info));
  REQUIRE(info.state != 0);

  const rex::X_HANDLE handle = rex::runtime::CreateModule(runtime, ModuleType::kUser);
  REQUIRE(handle != 0);
  {
    auto module = runtime.GetObjectTable().Get<ModuleObject>(handle);
    REQUIRE(module);
    module->user.module_format = ModuleFormat::kXEX;
    module->processor_module = std::move(xex);
  }
  rex::runtime::CloseOwned(runtime.GetObjectTable(), handle);
  CHECK(destroyed == 1);
  CHECK_FALSE(loaded_at_destroy);
  REQUIRE(heap->QueryRegionInfo(spec.load_address, &info));
  CHECK(info.state == 0);
}

REX_TEST_CASE("The loaded command line quotes the image name and appends the arguments",
              "[runtime][module]") {
  using rex::runtime::LoadedCommandLine;
  CHECK(LoadedCommandLine("game:\\title.xex", "") == "\"title.xex\"");
  CHECK(LoadedCommandLine("game:\\bin\\title.xex", "-level 2") == "\"title.xex\" -level 2");
  CHECK(LoadedCommandLine("game:\\default.xex", "") == "\"default.xex\"");
}

REX_TEST_CASE("The loaded command line fits the capacity for the longest image name",
              "[runtime][module]") {
  const std::string longest(kExLoadedImageNameSize - 1, 'a');
  CHECK(rex::runtime::LoadedCommandLine(longest, "-x").size() <
        rex::runtime::LoadedCommandLineCapacity("-x"));
}
