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

#include <rex/runtime.h>
#include <rex/runtime/module.h>
#include <rex/runtime/module_object.h>
#include <rex/runtime/object_table.h>
#include <rex/runtime/xex_module.h>
#include <rex/runtime/xmemory.h>

#include "core/xex_builder.h"
#include "kernel/kernel_fixture.h"

namespace {

using rex::runtime::ModuleFormat;
using rex::runtime::ModuleObject;
using rex::runtime::ModuleType;

class CountingModule : public rex::runtime::Module {
 public:
  CountingModule(rex::runtime::FunctionDispatcher* dispatcher, u32* destroyed)
      : Module(dispatcher), destroyed_(destroyed) {}
  ~CountingModule() override { ++*destroyed_; }
  const std::string& name() const override { return name_; }
  bool is_executable() const override { return false; }

 private:
  std::string name_ = "counting";
  u32* destroyed_;
};

class WatchedXEXModule : public rex::runtime::XEXModule {
 public:
  WatchedXEXModule(rex::Runtime& runtime, u32* destroyed, bool* loaded_at_destroy)
      : XEXModule(runtime.function_dispatcher(), runtime),
        destroyed_(destroyed),
        loaded_at_destroy_(loaded_at_destroy) {}
  ~WatchedXEXModule() override {
    ++*destroyed_;
    *loaded_at_destroy_ = loaded();
  }

 private:
  u32* destroyed_;
  bool* loaded_at_destroy_;
};

}  // namespace

REX_TEST_CASE("A user module object frees its loaded module when it is destroyed",
              "[runtime][module]") {
  rex::testing::KernelFixture fixture;
  rex::Runtime& runtime = fixture.runtime();
  u32 destroyed = 0;
  const rex::X_HANDLE handle = rex::runtime::CreateModule(runtime, ModuleType::User);
  REQUIRE(handle != 0);
  {
    auto module = runtime.object_table().Get<ModuleObject>(handle);
    REQUIRE(module);
    module->user.module_format = ModuleFormat::Elf;
    module->processor_module =
        std::make_unique<CountingModule>(runtime.function_dispatcher(), &destroyed);
  }
  CHECK(destroyed == 0);
  rex::runtime::CloseOwned(runtime.object_table(), handle);
  CHECK(destroyed == 1);
}

REX_TEST_CASE("A user module object unloads its XEX image before freeing the module",
              "[runtime][module]") {
  rex::testing::KernelFixture fixture;
  rex::Runtime& runtime = fixture.runtime();
  rex::test::XEXSpec spec;
  spec.image = rex::test::TestImage(0x1000);
  const auto file = rex::test::BuildXEX(spec);
  u32 destroyed = 0;
  bool loaded_at_destroy = true;
  auto xex = std::make_unique<WatchedXEXModule>(runtime, &destroyed, &loaded_at_destroy);
  REQUIRE(xex->Load("watched", "game:\\watched.xex", file.data(), file.size()));
  rex::runtime::BaseHeap* heap = runtime.memory()->LookupHeap(spec.load_address);
  rex::runtime::HeapAllocationInfo info{};
  REQUIRE(heap->QueryRegionInfo(spec.load_address, &info));
  REQUIRE(info.state != 0);

  const rex::X_HANDLE handle = rex::runtime::CreateModule(runtime, ModuleType::User);
  REQUIRE(handle != 0);
  {
    auto module = runtime.object_table().Get<ModuleObject>(handle);
    REQUIRE(module);
    module->user.module_format = ModuleFormat::XEX;
    module->processor_module = std::move(xex);
  }
  rex::runtime::CloseOwned(runtime.object_table(), handle);
  CHECK(destroyed == 1);
  CHECK_FALSE(loaded_at_destroy);
  REQUIRE(heap->QueryRegionInfo(spec.load_address, &info));
  CHECK(info.state == 0);
}
