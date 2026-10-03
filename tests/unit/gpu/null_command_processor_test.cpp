/**
 * @file        tests/unit/gpu/null_command_processor_test.cpp
 * @brief       rex-gpu-xenos with backend null runs PM4 without drawing
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <thread>

#include <rex/api.h>
#include <rex/filesystem.h>
#include <rex/gpu.h>
#include <rex/memory/utils.h>
#include <rex/mmio.h>
#include <rex/runtime.h>
#include <rex/runtime/xmemory.h>
#include <rex/xenos/xenos.h>

#include "test_memory.h"

namespace {

rex::Config NullConfig() {
  rex::Config config;
  config.paths.config_file.clear();
  config.modules = {"rex-filesystem", "rex-runtime", "rex-gpu-xenos"};
  config.gpu.backend = "null";
  return config;
}

struct Fixture {
  Fixture() {
    rex::Shutdown();
    rex::testing::ReleaseTestMemory();
  }
  ~Fixture() { rex::Shutdown(); }
};

void Put(rex::runtime::Memory& memory, rex::u32 physical, rex::u32 value) {
  rex::memory::store_and_swap<rex::u32>(memory.TranslatePhysical(physical), value);
}

rex::u32 Get(rex::runtime::Memory& memory, rex::u32 physical) {
  return rex::memory::load_and_swap<rex::u32>(memory.TranslatePhysical(physical));
}

constexpr rex::u32 kWritePointerRegister = 0x01C5;

}  // namespace

TEST_CASE_METHOD(Fixture, "backend null runs a MEM_WRITE and an EVENT_WRITE_SHD", "[gpu]") {
  auto api = rex::Init(NullConfig());
  REQUIRE(api);
  auto* backend = (*api)->Get<rex::gpu::IBackend>();
  REQUIRE(backend != nullptr);
  rex::Runtime runtime((*api)->config());
  rex::RuntimeConfig setup;
  setup.file_system = (*api)->Get<rex::filesystem::IFileSystem>();
  setup.gpu = backend;
  REQUIRE(runtime.Setup(setup));
  CHECK_FALSE(backend->has_presentation());

  auto& memory = *runtime.memory();
  const rex::u32 block = memory.SystemHeapAlloc(0x2000, 0x1000, rex::runtime::kSystemHeapPhysical);
  REQUIRE(block != 0);
  const rex::u32 ring = memory.GetPhysicalAddress(block);
  const rex::u32 writeback = ring + 0x1000;
  const rex::u32 target = ring + 0x1100;
  const auto swap = static_cast<rex::u32>(rex::xenos::Endian::k8in32);

  const rex::u32 packets[] = {
      rex::xenos::MakePacketType3(rex::xenos::PM4_MEM_WRITE, 2),
      target | swap,
      0xCAFEF00D,
      rex::xenos::MakePacketType3(rex::xenos::PM4_EVENT_WRITE_SHD, 3),
      0,
      (target + 4) | swap,
      0x12345678,
  };
  for (rex::u32 i = 0; i < std::size(packets); ++i) {
    Put(memory, ring + i * 4, packets[i]);
  }
  backend->InitializeRingBuffer(ring, 9);
  backend->EnableReadPointerWriteBack(writeback, 6);
  rex::detail::MMIOStore(rex::MMIOBlockBase(rex::MMIOBlock::GPU) + kWritePointerRegister * 4,
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

TEST_CASE_METHOD(Fixture, "A misspelled backend fails Init with its name", "[gpu]") {
  rex::Config config = NullConfig();
  config.gpu.backend = "nul";
  auto api = rex::Init(config);
  REQUIRE_FALSE(api);
  CHECK(api.error().message.find("nul") != std::string::npos);
}
