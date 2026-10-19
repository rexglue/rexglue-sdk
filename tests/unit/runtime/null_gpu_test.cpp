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

#include "rex_test.h"

#include <atomic>
#include <chrono>
#include <thread>

#include <rex/gpu/fixed_registers.h>
#include <rex/mmio.h>
#include <rex/runtime.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime/xmemory.h>

#include "kernel/kernel_fixture.h"
#include "runtime/null_gpu.h"

using rex::test::KernelFixture;

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

REX_TEST_CASE("NullGPU raises the vblank interrupt at the refresh rate", "[runtime]") {
  g_vblanks = 0;
  KernelFixture fixture;
  auto& runtime = fixture.GetRuntime();
  const rex::u32 callback = runtime.GetFunctionDispatcher()->AllocateThunk(&CountVblank, 0);
  REQUIRE(callback != 0);
  rex::runtime::NullGPU gpu;
  gpu.SetInterruptCallback(callback, 0x1234);
  REQUIRE(gpu.SetupGPU(*runtime.GetFunctionDispatcher(), runtime));
  const auto start = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(std::chrono::seconds(1));
  const std::chrono::duration<rex::f64> elapsed = std::chrono::steady_clock::now() - start;
  const int vblanks = g_vblanks;
  gpu.Shutdown();
  const rex::f64 expected = elapsed.count() * 60.0;
  CHECK(vblanks >= expected - 2.0);
  CHECK(vblanks <= expected + 2.0);
  CHECK(g_source == 0);
  CHECK(g_user_data == 0x1234);
}

REX_TEST_CASE("NullGPU without a callback raises nothing", "[runtime]") {
  g_vblanks = 0;
  KernelFixture fixture;
  auto& runtime = fixture.GetRuntime();
  rex::runtime::NullGPU gpu;
  REQUIRE(gpu.SetupGPU(*runtime.GetFunctionDispatcher(), runtime));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  gpu.Shutdown();
  CHECK(g_vblanks == 0);
}

REX_TEST_CASE("NullGPU writes the write pointer back as the read pointer", "[runtime]") {
  KernelFixture fixture;
  auto& runtime = fixture.GetRuntime();
  auto* memory = runtime.GetMemory();
  const rex::u32 ring = memory->SystemHeapAlloc(0x1000, 0x1000, rex::runtime::kSystemHeapPhysical);
  REQUIRE(ring != 0);
  const rex::u32 physical = memory->GetPhysicalAddress(ring);
  const rex::u32 writeback = physical + 0x800;
  rex::runtime::NullGPU gpu;
  REQUIRE(gpu.SetupGPU(*runtime.GetFunctionDispatcher(), runtime));
  gpu.InitializeRingBuffer(physical, 9);
  gpu.EnableReadPointerWriteBack(writeback, 6);
  const rex::u32 register_address =
      rex::MMIOBlockBase(rex::MMIOBlock::kGPU) + kWritePointerRegister * 4;
  rex::ppc::MMIOStore32(register_address, 0x20);
  CHECK(*memory->TranslatePhysical<const be_u32*>(writeback) == 0x20);
  CHECK(rex::ppc::MMIOLoad32(register_address) == 0x20);
  gpu.Shutdown();
  rex::ppc::MMIOStore32(register_address, 0x30);
  CHECK(*memory->TranslatePhysical<const be_u32*>(writeback) == 0x20);
}

REX_TEST_CASE("NullGPU has no presentation", "[runtime]") {
  rex::runtime::NullGPU gpu;
  CHECK_FALSE(gpu.HasPresentation());
  CHECK(gpu.GetPresenter() == nullptr);
  CHECK_FALSE(gpu.CreatePresenter());
  CHECK(gpu.InitializeShaderStorage({}, 0, true));
}

REX_TEST_CASE("The fixed registers report the display mode and canned values", "[runtime][gpu]") {
  rex::ui::VideoMode mode;
  mode.width = 1920;
  mode.height = 5000;
  using rex::gpu::ReadFixedRegister;
  using rex::xenos::Register;
  CHECK(ReadFixedRegister(static_cast<rex::u32>(Register::RB_EDRAM_TIMING), mode) == 0x08100748u);
  CHECK(ReadFixedRegister(static_cast<rex::u32>(Register::RB_BC_CONTROL), mode) == 0x0000200Eu);
  CHECK(ReadFixedRegister(static_cast<rex::u32>(Register::D1MODE_V_COUNTER), mode) == 0x0FFFu);
  CHECK(ReadFixedRegister(static_cast<rex::u32>(Register::D1MODE_VBLANK_VLINE_STATUS), mode) == 1u);
  CHECK(ReadFixedRegister(static_cast<rex::u32>(Register::AVIVO_D1MODE_VIEWPORT_SIZE), mode) ==
        ((1920u << 16) | 0x0FFFu));
  CHECK_FALSE(ReadFixedRegister(static_cast<rex::u32>(Register::CP_RB_WPTR), mode));
}
