/**
 * @file        tests/unit/gpu/xenos_base_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>

#include <rex/logging/logger.h>
#include <rex/xenos/register.h>
#include <rex/xenos/format.h>
#include <rex/xenos/packet.h>

#include "fake_driver_context.h"
#include "fake_title_context.h"
#include "gpu/xenos/null/null_xenos_graphics_driver.h"
#include "gpu/xenos/shared_memory.h"
#include "test_memory.h"

namespace {

using rex::test::FakeDriverContext;
using rex::test::FakeTitleContext;

struct Fixture {
  Fixture() { rex::test::ReleaseTestMemory(); }
  ~Fixture() { rex::test::ReleaseTestMemory(); }
};

constexpr rex::u32 kWritePointerRegister = 0x01C5;

void Put(rex::kernel::Memory& memory, rex::u32 physical, rex::u32 value) {
  *memory.TranslatePhysical<be_u32*>(physical) = value;
}

rex::u32 Get(rex::kernel::Memory& memory, rex::u32 physical) {
  return memory.TranslatePhysical<const be_u32*>(physical)->Load(std::memory_order_acquire);
}

template <typename Done>
bool WaitFor(Done done) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!done() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return done();
}

struct Ring {
  rex::u32 base = 0;
  rex::u32 writeback = 0;
  rex::u32 target = 0;
  rex::u32 count = 0;
};

Ring Submit(rex::kernel::Memory& memory, rex::gpu::null::NullXenosGraphicsDriver& driver,
            rex::xenos::Endian endian) {
  const rex::u32 block = memory.SystemHeapAlloc(0x2000, 0x1000, rex::kernel::kSystemHeapPhysical);
  Ring ring;
  ring.base = memory.GetPhysicalAddress(block);
  ring.writeback = ring.base + 0x1000;
  ring.target = ring.base + 0x1100;
  const auto swap = std::to_underlying(endian);
  const rex::u32 packets[] = {
      rex::xenos::BuildPacketType3(rex::xenos::PM4_MEM_WRITE, 2),
      ring.target | swap,
      0xCAFEF00D,
      rex::xenos::BuildPacketType3(rex::xenos::PM4_EVENT_WRITE_SHD, 3),
      0,
      (ring.target + 4) | swap,
      0x12345678,
  };
  ring.count = static_cast<rex::u32>(std::size(packets));
  for (rex::u32 i = 0; i < ring.count; ++i) {
    Put(memory, ring.base + i * 4, packets[i]);
  }
  driver.InitializeRingBuffer(ring.base, 9);
  driver.EnableReadPointerWriteBack(ring.writeback, 6);
  driver.WriteRegister(kWritePointerRegister, ring.count);
  return ring;
}

class TestSharedMemory final : public rex::gpu::SharedMemory {
 public:
  explicit TestSharedMemory(FakeTitleContext& title) : SharedMemory(title, title) {
    InitializeCommon();
  }

  rex::u32 uploads = 0;

 protected:
  bool UploadRanges(const std::vector<std::pair<rex::u32, rex::u32>>& ranges) override {
    ++uploads;
    for (const auto& [start, count] : ranges) {
      MarkRangeValid(start << GetPageSizeLog2(), count << GetPageSizeLog2(), false);
    }
    return true;
  }
};

}  // namespace

REX_TEST_CASE_METHOD(Fixture, "null command processor runs a ring", "[gpu][xenos]") {
  auto& memory = rex::test::GetTestMemory();
  FakeTitleContext title(memory);
  FakeDriverContext context;
  rex::gpu::null::NullXenosGraphicsDriver driver;
  REQUIRE(driver.Initialize(context));
  REQUIRE(driver.AttachTitle(rex::TitleInfo{}, title, title, title));

  const Ring ring = Submit(memory, driver, rex::xenos::Endian::k8in32);
  CHECK(WaitFor([&] { return Get(memory, ring.writeback) == ring.count; }));
  CHECK(Get(memory, ring.target) == 0xCAFEF00D);
  CHECK(Get(memory, ring.target + 4) == 0x12345678);

  driver.DetachTitle();
  driver.Shutdown();
}

REX_TEST_CASE_METHOD(Fixture, "fence write is release ordered", "[gpu][xenos]") {
  auto& memory = rex::test::GetTestMemory();
  FakeTitleContext title(memory);
  FakeDriverContext context;
  rex::gpu::null::NullXenosGraphicsDriver driver;
  REQUIRE(driver.Initialize(context));
  REQUIRE(driver.AttachTitle(rex::TitleInfo{}, title, title, title));

  const Ring ring = Submit(memory, driver, rex::xenos::Endian::k8in32);
  REQUIRE(WaitFor([&] { return Get(memory, ring.writeback) == ring.count; }));
  driver.DetachTitle();

  const auto stores = title.GetStores();
  const rex::u32 fence_address = ring.target + 4;
  const auto fence = std::ranges::find_if(
      stores, [&](const FakeTitleContext::Store& store) { return store.address == fence_address; });
  REQUIRE(fence != stores.end());
  CHECK(fence->value == 0x12345678);
  const auto writeback = std::ranges::find_if(stores, [&](const FakeTitleContext::Store& store) {
    return store.address == ring.writeback && store.value == ring.count;
  });
  REQUIRE(writeback != stores.end());
  CHECK(writeback->value == ring.count);
  CHECK(fence < writeback);
  driver.Shutdown();
}

REX_TEST_CASE_METHOD(Fixture, "write listener invalidates shared memory", "[gpu][xenos]") {
  auto& memory = rex::test::GetTestMemory();
  FakeTitleContext title(memory);
  TestSharedMemory shared(title);
  REQUIRE(title.listeners.size() == 1);

  constexpr rex::u32 kStart = 0x10000;
  constexpr rex::u32 kLength = 0x1000;
  REQUIRE(shared.RequestRange(kStart, kLength));
  CHECK(shared.uploads == 1);
  REQUIRE(shared.RequestRange(kStart, kLength));
  CHECK(shared.uploads == 1);

  title.listeners[0]->OnPhysicalWrite({kStart, kLength}, true);
  REQUIRE(shared.RequestRange(kStart, kLength));
  CHECK(shared.uploads == 2);
}
