/**
 * @file        tests/unit/runtime/null_gpu_test.cpp
 * @brief       runtime::NullGPU raises vblank and answers the ring buffer read pointer
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include <rex/memory/utils.h>
#include <rex/mmio.h>
#include <rex/runtime.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime/xmemory.h>

#include "kernel/kernel_fixture.h"
#include "runtime/null_gpu.h"

using rex::testing::KernelFixture;

namespace {

std::atomic<int> g_vblanks{0};
std::atomic<rex::u64> g_source{0xFFFF};
std::atomic<rex::u64> g_user_data{0};

void CountVblank(PPCContext& ctx, rex::u8*) {
  g_source = ctx.r3.u64;
  g_user_data = ctx.r4.u64;
  ++g_vblanks;
  ctx.r3.u64 = 0;
}

constexpr rex::u32 kWritePointerRegister = 0x01C5;

}  // namespace

TEST_CASE("NullGPU raises the vblank interrupt at the refresh rate", "[gpu]") {
  g_vblanks = 0;
  KernelFixture fixture;
  auto& runtime = fixture.runtime();
  const rex::u32 callback = runtime.function_dispatcher()->AllocateThunk(&CountVblank, 0);
  REQUIRE(callback != 0);
  rex::runtime::NullGPU gpu;
  gpu.SetInterruptCallback(callback, 0x1234);
  REQUIRE(gpu.SetupGuestGPU(*runtime.function_dispatcher(), runtime));
  std::this_thread::sleep_for(std::chrono::seconds(1));
  gpu.Shutdown();
  CHECK(g_vblanks >= 58);
  CHECK(g_vblanks <= 62);
  CHECK(g_source == 0);
  CHECK(g_user_data == 0x1234);
}

TEST_CASE("NullGPU without a callback raises nothing", "[gpu]") {
  g_vblanks = 0;
  KernelFixture fixture;
  auto& runtime = fixture.runtime();
  rex::runtime::NullGPU gpu;
  REQUIRE(gpu.SetupGuestGPU(*runtime.function_dispatcher(), runtime));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  gpu.Shutdown();
  CHECK(g_vblanks == 0);
}

TEST_CASE("NullGPU writes the write pointer back as the read pointer", "[gpu]") {
  KernelFixture fixture;
  auto& runtime = fixture.runtime();
  auto* memory = runtime.memory();
  const rex::u32 ring = memory->SystemHeapAlloc(0x1000, 0x1000, rex::runtime::kSystemHeapPhysical);
  REQUIRE(ring != 0);
  const rex::u32 physical = memory->GetPhysicalAddress(ring);
  const rex::u32 writeback = physical + 0x800;
  rex::runtime::NullGPU gpu;
  REQUIRE(gpu.SetupGuestGPU(*runtime.function_dispatcher(), runtime));
  gpu.InitializeRingBuffer(physical, 9);
  gpu.EnableReadPointerWriteBack(writeback, 6);
  const rex::u32 register_address =
      rex::MMIOBlockBase(rex::MMIOBlock::GPU) + kWritePointerRegister * 4;
  rex::detail::MMIOStore(register_address, 0x20);
  CHECK(rex::memory::load_and_swap<rex::u32>(memory->TranslatePhysical(writeback)) == 0x20);
  CHECK(rex::detail::MMIOLoad(register_address) == 0x20);
  gpu.Shutdown();
  rex::detail::MMIOStore(register_address, 0x30);
  CHECK(rex::memory::load_and_swap<rex::u32>(memory->TranslatePhysical(writeback)) == 0x20);
}

TEST_CASE("NullGPU has no presentation", "[gpu]") {
  rex::runtime::NullGPU gpu;
  CHECK_FALSE(gpu.has_presentation());
  CHECK(gpu.presenter() == nullptr);
  CHECK_FALSE(gpu.CreatePresenter());
  CHECK(gpu.InitializeShaderStorage({}, 0, true));
}
