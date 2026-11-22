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
#include <catch2/benchmark/catch_benchmark.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>

#include <rex/logging/log_category.h>
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

void Put(rex::kernel::GuestAddressSpace& memory, rex::u32 physical, rex::u32 value) {
  *memory.TranslatePhysical<be_u32*>(physical) = value;
}

rex::u32 Get(rex::kernel::GuestAddressSpace& memory, rex::u32 physical) {
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

Ring Submit(rex::kernel::GuestAddressSpace& memory, rex::gpu::null::NullXenosGraphicsDriver& driver,
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

  rex::u32 PageSizeLog2() const { return GetPageSizeLog2(); }

  rex::u32 uploads = 0;
  rex::u64 pages = 0;
  bool record = false;
  /** False leaves the uploaded pages invalid, so the next request uploads them again. */
  bool mark_valid = true;
  std::vector<std::pair<rex::u32, rex::u32>> recorded;

 protected:
  bool UploadRanges(const std::vector<std::pair<rex::u32, rex::u32>>& ranges) override {
    ++uploads;
    for (const auto& [start, count] : ranges) {
      pages += count;
      if (mark_valid) {
        MarkRangeValid(start << GetPageSizeLog2(), count << GetPageSizeLog2(), false);
      }
    }
    if (record) {
      recorded.insert(recorded.end(), ranges.begin(), ranges.end());
    }
    return true;
  }
};

/** One range requested through RequestRange and through RequestRanges on fresh shared memory. */
struct RequestOutcome {
  bool result = false;
  rex::u32 uploads = 0;
  rex::u64 pages = 0;
  std::vector<std::pair<rex::u32, rex::u32>> recorded;
  bool operator==(const RequestOutcome&) const = default;
};

RequestOutcome Outcome(FakeTitleContext& title, rex::u32 start, rex::u32 length, bool single) {
  TestSharedMemory shared(title);
  shared.record = true;
  RequestOutcome outcome;
  if (single) {
    outcome.result = shared.RequestRange(start, length);
  } else {
    const std::pair<rex::u32, rex::u32> range(start, length);
    outcome.result = shared.RequestRanges(&range, 1);
  }
  outcome.uploads = shared.uploads;
  outcome.pages = shared.pages;
  outcome.recorded = shared.recorded;
  return outcome;
}

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

REX_TEST_CASE_METHOD(Fixture, "RequestRange uploads the pages RequestRanges uploads for one range",
                     "[gpu][xenos][shared_memory]") {
  auto& memory = rex::test::GetTestMemory();
  FakeTitleContext title(memory);
  constexpr rex::u32 kSize = rex::gpu::SharedMemory::kBufferSize;
  const std::pair<rex::u32, rex::u32> cases[] = {
      {0x10000, 0x10000},
      {0x10000, 0x1000},
      {0x10001, 1},
      {0x10FFF, 2},
      {0x20000, 0},
      {0, 0},
      {kSize, 0},
      {kSize + 1, 0},
      {kSize - 0x1000, 0x1000},
      {kSize - 0x1000, 0x1001},
      {kSize, 1},
      {kSize + 1, 1},
      {0x7FFFFFFF, 0x1000},
      {0xFFFFFFFF, 2},
      {kSize - 1, 1},
      {0x100, 0xFFFFFFFF},
  };
  for (const auto& [start, length] : cases) {
    INFO("start " << start << " length " << length);
    const RequestOutcome single = Outcome(title, start, length, true);
    const RequestOutcome vector_path = Outcome(title, start, length, false);
    CHECK(single == vector_path);
    CHECK(single.result == (length == 0 || (start <= kSize && kSize - start >= length)));
  }
}

REX_TEST_CASE_METHOD(Fixture, "RequestRange of valid and invalid ranges, with the work counted",
                     "[gpu][xenos][shared_memory][benchmark]") {
  auto& memory = rex::test::GetTestMemory();
  FakeTitleContext title(memory);
  TestSharedMemory shared(title);
  constexpr rex::u32 kRange = 0x10000;
  constexpr rex::u32 kValidStart = 0x100000;
  constexpr rex::u32 kInvalidStart = 0x1000000;
  REQUIRE(shared.RequestRange(kValidStart, kRange));
  rex::u64 valid_calls = 0;
  const rex::u32 valid_uploads_before = shared.uploads;
  BENCHMARK("baseline: an empty call") {
    return valid_calls;
  };
  BENCHMARK("RequestRange, valid 64 KiB range") {
    ++valid_calls;
    return shared.RequestRange(kValidStart, kRange);
  };
  const rex::u32 valid_uploads = shared.uploads - valid_uploads_before;
  // The uploaded pages stay invalid, so every request below uploads its range again.
  shared.mark_valid = false;
  rex::u64 invalid_calls = 0;
  rex::u64 invalid_succeeded = 0;
  const rex::u64 invalid_pages_before = shared.pages;
  BENCHMARK("RequestRange, invalid 64 KiB range that uploads") {
    ++invalid_calls;
    const bool ok = shared.RequestRange(kInvalidStart, kRange);
    invalid_succeeded += ok;
    return ok;
  };
  const rex::u64 invalid_pages = shared.pages - invalid_pages_before;
  rex::u64 small_calls = 0;
  const rex::u64 small_pages_before = shared.pages;
  BENCHMARK("RequestRange, invalid 4 KiB range that uploads") {
    ++small_calls;
    return shared.RequestRange(kInvalidStart + 0x400000, 0x1000);
  };
  const rex::u64 small_pages = shared.pages - small_pages_before;
  WARN("valid range: " << valid_calls << " calls, " << valid_uploads << " uploads; invalid 64 KiB: "
                       << invalid_calls << " calls, " << invalid_succeeded << " succeeded, "
                       << invalid_pages << " pages uploaded; invalid 4 KiB: " << small_calls
                       << " calls, " << small_pages << " pages uploaded");
  CHECK(valid_uploads == 0);
  CHECK(invalid_succeeded == invalid_calls);
  CHECK(invalid_pages == invalid_calls * (kRange >> shared.PageSizeLog2()));
  CHECK(small_pages == small_calls * (0x1000 >> shared.PageSizeLog2()));
}

namespace {

struct WaitRings {
  rex::u32 first = 0;
  rex::u32 first_count = 0;
  rex::u32 second = 0;
  rex::u32 second_count = 0;
  rex::u32 gate = 0;
  rex::u32 reached_marker = 0;
  rex::u32 first_marker = 0;
  rex::u32 second_marker = 0;
};

constexpr rex::u32 kReachedValue = 0x0F0F0F0F;

// The first ring stores a reached marker, waits on memory the second step writes, then stores a
// marker. The second ring only stores a marker. The processor runs a ring up to its write pointer
// before it applies a pending ring change, so once the reached marker is stored the processor is
// in the wait.
WaitRings BuildWaitRings(rex::kernel::GuestAddressSpace& memory) {
  const rex::u32 block = memory.SystemHeapAlloc(0x2000, 0x1000, rex::kernel::kSystemHeapPhysical);
  WaitRings rings;
  const rex::u32 base = memory.GetPhysicalAddress(block);
  rings.first = base;
  rings.second = base + 0x400;
  rings.gate = base + 0x1200;
  rings.first_marker = base + 0x1100;
  rings.second_marker = base + 0x1104;
  rings.reached_marker = base + 0x1108;
  const auto swap = std::to_underlying(rex::xenos::Endian::k8in32);
  const rex::u32 first[] = {
      rex::xenos::BuildPacketType3(rex::xenos::PM4_MEM_WRITE, 2),
      rings.reached_marker | swap,
      kReachedValue,
      rex::xenos::BuildPacketType3(rex::xenos::PM4_WAIT_REG_MEM, 5),
      0x13,
      rings.gate | swap,
      1,
      0xFFFFFFFF,
      0x0A,
      rex::xenos::BuildPacketType3(rex::xenos::PM4_MEM_WRITE, 2),
      rings.first_marker | swap,
      0x11111111,
  };
  const rex::u32 second[] = {
      rex::xenos::BuildPacketType3(rex::xenos::PM4_MEM_WRITE, 2),
      rings.second_marker | swap,
      0x22222222,
  };
  rings.first_count = static_cast<rex::u32>(std::size(first));
  rings.second_count = static_cast<rex::u32>(std::size(second));
  for (rex::u32 i = 0; i < rings.first_count; ++i) {
    Put(memory, rings.first + i * 4, first[i]);
  }
  for (rex::u32 i = 0; i < rings.second_count; ++i) {
    Put(memory, rings.second + i * 4, second[i]);
  }
  return rings;
}

}  // namespace

REX_TEST_CASE_METHOD(Fixture,
                     "a ring buffer set up while the processor waits on memory does not block the "
                     "caller",
                     "[gpu][xenos]") {
  auto& memory = rex::test::GetTestMemory();
  FakeTitleContext title(memory);
  FakeDriverContext context;
  rex::gpu::null::NullXenosGraphicsDriver driver;
  REQUIRE(driver.Initialize(context));
  REQUIRE(driver.AttachTitle(rex::TitleInfo{}, title, title, title));

  const WaitRings rings = BuildWaitRings(memory);
  driver.InitializeRingBuffer(rings.first, 9);
  driver.WriteRegister(kWritePointerRegister, rings.first_count);
  REQUIRE(WaitFor([&] { return Get(memory, rings.reached_marker) == kReachedValue; }));

  std::atomic<bool> returned{false};
  std::thread guest([&] {
    driver.InitializeRingBuffer(rings.second, 9);
    returned.store(true);
    Put(memory, rings.gate, 1);
  });
  const bool call_returned = WaitFor([&] { return returned.load(); });
  CHECK(call_returned);
  if (!call_returned) {
    Put(memory, rings.gate, 1);
  }
  guest.join();
  CHECK(WaitFor([&] { return Get(memory, rings.first_marker) == 0x11111111; }));

  driver.DetachTitle();
  driver.Shutdown();
}

REX_TEST_CASE_METHOD(Fixture,
                     "a ring buffer set up during a memory wait applies before the next packet",
                     "[gpu][xenos]") {
  auto& memory = rex::test::GetTestMemory();
  FakeTitleContext title(memory);
  FakeDriverContext context;
  rex::gpu::null::NullXenosGraphicsDriver driver;
  REQUIRE(driver.Initialize(context));
  REQUIRE(driver.AttachTitle(rex::TitleInfo{}, title, title, title));

  const WaitRings rings = BuildWaitRings(memory);
  driver.InitializeRingBuffer(rings.first, 9);
  driver.WriteRegister(kWritePointerRegister, rings.first_count);
  REQUIRE(WaitFor([&] { return Get(memory, rings.reached_marker) == kReachedValue; }));

  driver.InitializeRingBuffer(rings.second, 9);
  driver.WriteRegister(kWritePointerRegister, rings.second_count);
  Put(memory, rings.gate, 1);

  CHECK(WaitFor([&] { return Get(memory, rings.first_marker) == 0x11111111; }));
  CHECK(WaitFor([&] { return Get(memory, rings.second_marker) == 0x22222222; }));

  driver.DetachTitle();
  driver.Shutdown();
}

REX_TEST_CASE_METHOD(Fixture, "IsRangeValid is false for a requested range after a CPU write",
                     "[gpu][xenos][shared_memory]") {
  auto& memory = rex::test::GetTestMemory();
  FakeTitleContext title(memory);
  TestSharedMemory shared(title);
  REQUIRE(title.listeners.size() == 1);

  constexpr rex::u32 kWritten = 0x100000;
  constexpr rex::u32 kUntouched = 0x200000;
  constexpr rex::u32 kNeverRequested = 0x300000;
  constexpr rex::u32 kLength = 0x10000;
  REQUIRE(shared.RequestRange(kWritten, kLength));
  REQUIRE(shared.RequestRange(kUntouched, kLength));

  {
    auto lock = shared.AcquireLock();
    CHECK(shared.IsRangeValid(lock, kWritten, kLength));
    CHECK(shared.IsRangeValid(lock, kUntouched, kLength));
    CHECK_FALSE(shared.IsRangeValid(lock, kNeverRequested, kLength));
  }

  title.listeners[0]->OnPhysicalWrite({kWritten + 0x8000, 0x10}, true);
  {
    auto lock = shared.AcquireLock();
    CHECK_FALSE(shared.IsRangeValid(lock, kWritten, kLength));
    CHECK(shared.IsRangeValid(lock, kUntouched, kLength));
  }

  REQUIRE(shared.RequestRange(kWritten, kLength));
  {
    auto lock = shared.AcquireLock();
    CHECK(shared.IsRangeValid(lock, kWritten, kLength));
  }
}
