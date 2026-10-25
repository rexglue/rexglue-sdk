/**
 * @file        tests/unit/gpu/null_command_processor_test.cpp
 * @brief       rexgpu-xenos with backend null runs PM4 without drawing
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <rex/api.h>
#include <rex/filesystem.h>
#include <rex/gpu.h>
#include <rex/mmio.h>
#include <rex/runtime.h>
#include <rex/runtime/xmemory.h>
#include <rex/xenos/xenos.h>

namespace {

rex::Config NullConfig() {
  rex::Config config;
  config.paths.config_file.clear();
  config.modules = {"rexfilesystem", "rexruntime", "rexgpu-xenos"};
  config.gpu.backend = "null";
  return config;
}

struct Fixture {
  Fixture() { rex::Shutdown(); }
  ~Fixture() { rex::Shutdown(); }
};

void Put(rex::runtime::Memory& memory, rex::u32 physical, rex::u32 value) {
  *memory.TranslatePhysical<be_u32*>(physical) = value;
}

rex::u32 Get(rex::runtime::Memory& memory, rex::u32 physical) {
  return memory.TranslatePhysical<const be_u32*>(physical)->Load(std::memory_order_acquire);
}

constexpr rex::u32 kWritePointerRegister = 0x01C5;

}  // namespace

REX_TEST_CASE_METHOD(Fixture, "backend null runs a MEM_WRITE and an EVENT_WRITE_SHD", "[gpu]") {
  auto api = rex::Init(NullConfig());
  REQUIRE(api);
  auto* backend = (*api)->Get<rex::gpu::IGPUBackend>();
  REQUIRE(backend != nullptr);
  rex::Runtime runtime((*api)->GetConfig());
  rex::RuntimeDesc setup;
  setup.file_system = (*api)->Get<rex::filesystem::IFileSystem>();
  setup.gpu = backend;
  REQUIRE(runtime.Setup(setup));
  CHECK_FALSE(backend->HasPresentation());

  auto& memory = *runtime.GetMemory();
  const rex::u32 block = memory.SystemHeapAlloc(0x2000, 0x1000, rex::runtime::kSystemHeapPhysical);
  REQUIRE(block != 0);
  const rex::u32 ring = memory.GetPhysicalAddress(block);
  const rex::u32 writeback = ring + 0x1000;
  const rex::u32 target = ring + 0x1100;
  const auto swap = std::to_underlying(rex::xenos::Endian::k8in32);

  const rex::u32 packets[] = {
      rex::xenos::BuildPacketType3(rex::xenos::PM4_MEM_WRITE, 2),
      target | swap,
      0xCAFEF00D,
      rex::xenos::BuildPacketType3(rex::xenos::PM4_EVENT_WRITE_SHD, 3),
      0,
      (target + 4) | swap,
      0x12345678,
  };
  for (rex::u32 i = 0; i < std::size(packets); ++i) {
    Put(memory, ring + i * 4, packets[i]);
  }
  backend->InitializeRingBuffer(ring, 9);
  backend->EnableReadPointerWriteBack(writeback, 6);
  rex::ppc::MMIOStore32(rex::MMIOBlockBase(rex::MMIOBlock::kGPU) + kWritePointerRegister * 4,
                        std::size(packets));

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (Get(memory, writeback) != std::size(packets) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(Get(memory, writeback) == std::size(packets));
  CHECK(Get(memory, target) == 0xCAFEF00D);
  CHECK(Get(memory, target + 4) == 0x12345678);
  runtime.Shutdown();
}

REX_TEST_CASE_METHOD(Fixture, "A mistyped backend name fails Init with its name", "[gpu]") {
  rex::Config config = NullConfig();
  config.gpu.backend = "nul";
  auto api = rex::Init(config);
  REQUIRE_FALSE(api);
  CHECK(api.error().message.find("nul") != std::string::npos);
}

REX_TEST_CASE_METHOD(Fixture, "Setting the interrupt callback while vblanks dispatch is safe",
                     "[gpu]") {
  auto api = rex::Init(NullConfig());
  REQUIRE(api);
  auto* backend = (*api)->Get<rex::gpu::IGPUBackend>();
  REQUIRE(backend != nullptr);
  rex::Runtime runtime((*api)->GetConfig());
  rex::RuntimeDesc setup;
  setup.file_system = (*api)->Get<rex::filesystem::IFileSystem>();
  setup.gpu = backend;
  REQUIRE(runtime.Setup(setup));
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
  for (rex::u32 i = 0; std::chrono::steady_clock::now() < until; ++i) {
    backend->SetInterruptCallback(0, i);
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  runtime.Shutdown();
}

REX_TEST_CASE_METHOD(Fixture, "Shutdown stops vblanks before the command processor goes away",
                     "[gpu]") {
  for (rex::u32 cycle = 0; cycle < 10; ++cycle) {
    rex::Config config = NullConfig();
    config.cvars.values.push_back({"gpu.vsync", "false"});
    auto api = rex::Init(config);
    REQUIRE(api);
    auto* backend = (*api)->Get<rex::gpu::IGPUBackend>();
    REQUIRE(backend != nullptr);
    rex::Runtime runtime((*api)->GetConfig());
    rex::RuntimeDesc setup;
    setup.file_system = (*api)->Get<rex::filesystem::IFileSystem>();
    setup.gpu = backend;
    REQUIRE(runtime.Setup(setup));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    runtime.Shutdown();
    rex::Shutdown();
    rex::test::ReleaseTestMemory();
  }
}

REX_TEST_CASE_METHOD(Fixture, "The command processor shuts down promptly while it waits idle",
                     "[gpu]") {
  constexpr auto kIdleTime = std::chrono::milliseconds(200);
  constexpr auto kShutdownBound = std::chrono::seconds(2);

  auto api = rex::Init(NullConfig());
  REQUIRE(api);
  auto* backend = (*api)->Get<rex::gpu::IGPUBackend>();
  REQUIRE(backend != nullptr);
  rex::Runtime runtime((*api)->GetConfig());
  rex::RuntimeDesc setup;
  setup.file_system = (*api)->Get<rex::filesystem::IFileSystem>();
  setup.gpu = backend;
  REQUIRE(runtime.Setup(setup));

  std::this_thread::sleep_for(kIdleTime);
  const auto start = std::chrono::steady_clock::now();
  runtime.Shutdown();
  CHECK(std::chrono::steady_clock::now() - start < kShutdownBound);
}
