/**
 * @file        tests/unit/gpu/fence_endianness_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <array>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>

#include <rex/xenos/register.h>
#include <rex/xenos/format.h>
#include <rex/xenos/packet.h>

#include "fake_driver_context.h"
#include "fake_title_context.h"
#include "gpu/xenos/null/null_xenos_graphics_driver.h"
#include "test_memory.h"

namespace {

using rex::xenos::Endian;

struct Fixture {
  Fixture() { rex::test::ReleaseTestMemory(); }
  ~Fixture() { rex::test::ReleaseTestMemory(); }
};

constexpr std::array<Endian, 4> kEndians = {Endian::kNone, Endian::k8in16, Endian::k8in32,
                                            Endian::k16in32};
constexpr rex::u32 kWritePointerRegister = 0x01C5;
constexpr rex::u32 kSourceValue = 0x11223344;
constexpr rex::u32 kPolledRef = 0x5566AA77;

void Put(rex::kernel::Memory& memory, rex::u32 physical, rex::u32 value) {
  *memory.TranslatePhysical<be_u32*>(physical) = value;
}

rex::u32 GetBig(rex::kernel::Memory& memory, rex::u32 physical) {
  return memory.TranslatePhysical<const be_u32*>(physical)->Load(std::memory_order_acquire);
}

rex::u32 GetNative(rex::kernel::Memory& memory, rex::u32 physical) {
  return *memory.TranslatePhysical<const rex::u32*>(physical);
}

}  // namespace

REX_TEST_CASE_METHOD(Fixture, "packets write and poll guest memory in every endian mode", "[gpu]") {
  auto& memory = rex::test::GetTestMemory();
  rex::test::FakeTitleContext title(memory);
  rex::test::FakeDriverContext context;
  rex::gpu::null::NullXenosGraphicsDriver driver;
  REQUIRE(driver.Initialize(context));
  REQUIRE(driver.AttachTitle(rex::TitleInfo{}, title, title, title));

  const rex::u32 block = memory.SystemHeapAlloc(0x4000, 0x1000, rex::kernel::kSystemHeapPhysical);
  REQUIRE(block != 0);
  const rex::u32 ring = memory.GetPhysicalAddress(block);
  const rex::u32 writeback = ring + 0x1000;
  const rex::u32 scratch = ring + 0x1040;
  const rex::u32 area = ring + 0x2000;
  constexpr rex::u32 kSourceRegister = std::to_underlying(rex::xenos::Register::SCRATCH_REG1);
  constexpr rex::u32 kStride = 0x40;

  driver.WriteRegister(kSourceRegister, kSourceValue);
  driver.WriteRegister(std::to_underlying(rex::xenos::Register::SCRATCH_ADDR), scratch);
  driver.WriteRegister(std::to_underlying(rex::xenos::Register::SCRATCH_UMSK), 1);

  std::vector<rex::u32> packets;
  for (rex::u32 i = 0; i < kEndians.size(); ++i) {
    const auto endian = std::to_underlying(kEndians[i]);
    const rex::u32 base = area + i * kStride;
    const rex::u32 poll = base + 0x00;
    *memory.TranslatePhysical<rex::u32*>(poll) = rex::xenos::GPUSwap(kPolledRef, kEndians[i]);
    packets.insert(packets.end(),
                   {
                       rex::xenos::BuildPacketType3(rex::xenos::PM4_MEM_WRITE, 2),
                       (base + 0x04) | endian,
                       0xCAFEF00D,
                       rex::xenos::BuildPacketType3(rex::xenos::PM4_EVENT_WRITE_SHD, 3),
                       0,
                       (base + 0x08) | endian,
                       0x12345678,
                       rex::xenos::BuildPacketType3(rex::xenos::PM4_REG_TO_MEM, 2),
                       kSourceRegister,
                       (base + 0x0C) | endian,
                       rex::xenos::BuildPacketType3(rex::xenos::PM4_WAIT_REG_MEM, 5),
                       0x13,
                       poll | endian,
                       kPolledRef,
                       0xFFFFFFFF,
                       0,
                       rex::xenos::BuildPacketType3(rex::xenos::PM4_COND_WRITE, 6),
                       0x113,
                       poll | endian,
                       kPolledRef,
                       0xFFFFFFFF,
                       (base + 0x10) | endian,
                       0x0BADF00D,
                   });
  }
  packets.insert(packets.end(), {
                                    rex::xenos::BuildPacketType3(rex::xenos::PM4_COND_WRITE, 6),
                                    0x7,
                                    0,
                                    0,
                                    0,
                                    std::to_underlying(rex::xenos::Register::SCRATCH_REG0),
                                    0xA1B2C3D4,
                                });
  for (rex::u32 i = 0; i < packets.size(); ++i) {
    Put(memory, ring + i * 4, packets[i]);
  }
  driver.InitializeRingBuffer(ring, 9);
  driver.EnableReadPointerWriteBack(writeback, 6);
  driver.WriteRegister(kWritePointerRegister, static_cast<rex::u32>(packets.size()));

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (GetBig(memory, writeback) != packets.size() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  REQUIRE(GetBig(memory, writeback) == packets.size());

  CHECK(GetBig(memory, scratch) == 0xA1B2C3D4);
  for (rex::u32 i = 0; i < kEndians.size(); ++i) {
    CAPTURE(i);
    const Endian endian = kEndians[i];
    const rex::u32 base = area + i * kStride;
    CHECK(GetNative(memory, base + 0x04) == rex::xenos::GPUSwap(0xCAFEF00Du, endian));
    CHECK(GetNative(memory, base + 0x08) == rex::xenos::GPUSwap(0x12345678u, endian));
    CHECK(GetNative(memory, base + 0x0C) == rex::xenos::GPUSwap(kSourceValue, endian));
    CHECK(GetNative(memory, base + 0x10) == rex::xenos::GPUSwap(0x0BADF00Du, endian));
  }
  driver.DetachTitle();
  driver.Shutdown();
}
