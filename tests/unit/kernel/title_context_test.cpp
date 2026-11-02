/**
 * @file        tests/unit/kernel/title_context_test.cpp
 * @brief       The title context of a running TitleRuntime
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>

#include <rex/kernel/title_runtime.h>
#include <rex/xam/achievement_manager.h>
#include <rex/kernel/heap.h>
#include <rex/title.h>

#include "../kernel/kernel_fixture.h"
#include "kernel/title_context.h"
#include "temp_folder.h"
#include "kernel/title_state.h"

namespace {

using namespace std::chrono_literals;

struct CountingListener final : rex::IPhysicalWriteListener {
  rex::PhysicalRange OnPhysicalWrite(rex::PhysicalRange range, bool) override {
    ++calls;
    last = range;
    return {0, 0xFFFFFFFF};
  }
  std::atomic<int> calls{0};
  rex::PhysicalRange last;
};

class PhysicalBlock {
 public:
  PhysicalBlock(rex::kernel::Memory& memory, rex::u32 size) : memory_(memory) {
    address_ = memory.SystemHeapAlloc(size, 0x1000, rex::kernel::kSystemHeapPhysical);
    REQUIRE(address_ != 0);
    physical_ = memory.GetPhysicalAddress(address_);
  }
  ~PhysicalBlock() { memory_.SystemHeapFree(address_); }

  rex::u32 GetPhysical() const { return physical_; }

 private:
  rex::kernel::Memory& memory_;
  rex::u32 address_ = 0;
  rex::u32 physical_ = 0;
};

}  // namespace

REX_TEST_CASE("physical copy checks range", "[kernel][title_context]") {
  rex::test::KernelFixture fixture;
  rex::kernel::TitleContext context(fixture.GetRuntime());
  PhysicalBlock block(*StateOf(fixture.GetRuntime()).GetMemory(), 0x1000);

  std::array<std::byte, 16> in;
  in.fill(std::byte{0x5A});
  CHECK(context.CopyToPhysical(block.GetPhysical(), in).has_value());
  std::array<std::byte, 16> out{};
  CHECK(context.CopyFromPhysical(block.GetPhysical(), out).has_value());
  CHECK(out == in);

  CHECK_FALSE(context.CopyFromPhysical(0x1FFFFFF8, out).has_value());
  CHECK_FALSE(context.CopyToPhysical(0x20000000, in).has_value());
  CHECK_FALSE(context.GetReadOnlyPhysicalSpan({0x1FFFFFF0, 0x20}).has_value());
  CHECK_FALSE(context.GetWritablePhysicalSpan({0x1FFFFFF0, 0x20}).has_value());
  const auto store_out = context.StoreRelease32(0x1FFFFFFC + 4, 1);
  REQUIRE_FALSE(store_out.has_value());
  CHECK(store_out.error().message.contains("outside physical memory"));
  const auto load_out = context.LoadAcquire32(0x20000000);
  REQUIRE_FALSE(load_out.has_value());
  CHECK(load_out.error().message.contains("outside physical memory"));
  const auto store_unaligned = context.StoreRelease32(block.GetPhysical() + 1, 1);
  REQUIRE_FALSE(store_unaligned.has_value());
  CHECK(store_unaligned.error().message.contains("not 4 byte aligned"));
  const auto load_unaligned = context.LoadAcquire32(block.GetPhysical() + 2);
  REQUIRE_FALSE(load_unaligned.has_value());
  CHECK(load_unaligned.error().message.contains("not 4 byte aligned"));
  CHECK(context.GetReadOnlyPhysicalSpan({block.GetPhysical(), 0x40}).has_value());
}

REX_TEST_CASE("write listener fires once per watch", "[kernel][title_context]") {
  rex::test::KernelFixture fixture;
  rex::kernel::TitleContext context(fixture.GetRuntime());
  PhysicalBlock block(*StateOf(fixture.GetRuntime()).GetMemory(), 0x1000);
  CountingListener listener;
  rex::ScopeGuard handle = context.AddWriteListener(listener);

  const std::array<std::byte, 4> data{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  context.WatchPhysicalWrites({block.GetPhysical(), 0x1000});
  REQUIRE(context.CopyToPhysical(block.GetPhysical(), data).has_value());
  CHECK(listener.calls.load() == 1);
  REQUIRE(context.CopyToPhysical(block.GetPhysical(), data).has_value());
  CHECK(listener.calls.load() == 1);
  context.WatchPhysicalWrites({block.GetPhysical(), 0x1000});
  REQUIRE(context.CopyToPhysical(block.GetPhysical(), data).has_value());
  CHECK(listener.calls.load() == 2);
}

REX_TEST_CASE("release store seen by acquire load", "[kernel][title_context]") {
  rex::test::KernelFixture fixture;
  rex::kernel::TitleContext context(fixture.GetRuntime());
  constexpr rex::u32 kRounds = 10000;
  PhysicalBlock block(*StateOf(fixture.GetRuntime()).GetMemory(), 0x20000);
  const rex::u32 base = block.GetPhysical();
  auto span = context.GetWritablePhysicalSpan({base, kRounds * 8});
  REQUIRE(span.has_value());
  std::memset(span->data(), 0, span->size());

  std::atomic<bool> ordered{true};
  std::thread reader([&] {
    for (rex::u32 i = 0; i < kRounds; ++i) {
      const rex::u32 flag = base + i * 8 + 4;
      while (true) {
        auto value = context.LoadAcquire32(flag);
        if (value.has_value() && *value == 1) {
          break;
        }
      }
      auto data = context.LoadAcquire32(base + i * 8);
      if (!data.has_value() || *data != i + 1) {
        ordered = false;
      }
    }
  });
  auto* host = StateOf(fixture.GetRuntime()).GetMemory();
  for (rex::u32 i = 0; i < kRounds; ++i) {
    host->TranslatePhysical<rex::be_u32*>(base + i * 8)->Store(i + 1, std::memory_order_relaxed);
    REQUIRE(context.StoreRelease32(base + i * 8 + 4, 1).has_value());
  }
  reader.join();
  CHECK(ordered.load());
}

REX_TEST_CASE("interrupt refused on a foreign thread", "[kernel][title_context]") {
  rex::test::KernelFixture fixture;
  rex::kernel::TitleContext context(fixture.GetRuntime());
  CHECK_FALSE(context.RaiseGraphicsInterrupt(0, 2).has_value());

  std::atomic<int> host_result{-1};
  auto thread = context.StartHostThread("title context test", [&](std::stop_token) {
    host_result = context.RaiseGraphicsInterrupt(0, 2).has_value() ? 1 : 0;
  });
  REQUIRE(thread.has_value());
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (host_result.load() < 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  CHECK(host_result.load() == 1);
}

REX_TEST_CASE("handles released before detach returns", "[kernel][title_context]") {
  rex::test::KernelFixture fixture;
  rex::kernel::TitleContext context(fixture.GetRuntime());
  PhysicalBlock block(*StateOf(fixture.GetRuntime()).GetMemory(), 0x1000);
  CountingListener listener;
  auto handle = std::make_unique<rex::ScopeGuard>(context.AddWriteListener(listener));
  context.WatchPhysicalWrites({block.GetPhysical(), 0x1000});
  std::atomic<bool> releasing{false};
  std::thread releaser([&] {
    std::this_thread::sleep_for(50ms);
    releasing = true;
    handle.reset();
  });
  context.Detach();
  CHECK(releasing.load());
  const std::array<std::byte, 4> data{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  REQUIRE(context.CopyToPhysical(block.GetPhysical(), data).has_value());
  CHECK(listener.calls.load() == 0);
  releaser.join();
}

REX_TEST_CASE("achievements listed and icon found", "[kernel][title_context]") {
  rex::test::KernelFixture fixture;
  rex::test::TempFolder folder("rexglue_title_context_");
  const auto icon_path = folder.path / "icon.png";
  {
    std::ofstream icon(icon_path, std::ios::binary);
    icon.write("\x89PNG", 4);
  }
  rex::Achievement achievement;
  achievement.id = 7;
  achievement.label = "First";
  achievement.description = "Done";
  achievement.unachieved_description = "Not yet";
  achievement.image_id = 70;
  achievement.gamerscore = 25;
  achievement.icon_path = icon_path.string();
  REQUIRE(
      fixture.GetRuntime().GetAchievementManager().RegisterAchievement(achievement).has_value());

  rex::kernel::TitleContext context(fixture.GetRuntime());
  const auto list = context.GetAchievements();
  REQUIRE(list.size() == 1);
  CHECK(list[0].id == 7);
  CHECK(list[0].label == "First");
  CHECK(list[0].GetDescription() == "Not yet");
  CHECK(list[0].gamerscore == 25);
  CHECK_FALSE(list[0].unlocked);

  auto icon = context.GetAchievementIcon(70);
  REQUIRE(icon.has_value());
  CHECK(icon->size() == 4);
  CHECK_FALSE(context.GetAchievementIcon(71).has_value());
}

REX_TEST_CASE("an achievement shows the text of its lock state", "[kernel][title_context]") {
  rex::Achievement achievement;
  achievement.description = "Done";
  achievement.unachieved_description = "Not yet";
  CHECK(achievement.GetDescription() == "Not yet");
  achievement.unlocked = true;
  CHECK(achievement.GetDescription() == "Done");
}
