/**
 * @file        tests/unit/runtime/runtime_interfaces_test.cpp
 * @brief       How guest threads, workers and modules reach the runtime
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
#include <cstring>
#include <memory>
#include <optional>
#include <thread>

#include <rex/api.h>
#include <rex/runtime.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime/object_table.h>
#include <rex/runtime/process.h>
#include <rex/runtime/thread_registry.h>
#include <rex/runtime/thread_state.h>
#include <rex/runtime/xthread.h>

#include "../kernel/kernel_fixture.h"

using rex::X_STATUS;

namespace {

std::atomic<rex::Runtime*> g_seen_runtime{nullptr};

void RecordCurrent(PPCContext& ctx, u8*) {
  g_seen_runtime = &rex::runtime::GetCurrent();
  ctx.r3.u64 = 0;
}

}  // namespace

REX_TEST_CASE("Current returns the runtime on a guest thread", "[runtime][current]") {
  rex::testing::KernelFixture fixture;
  rex::Runtime* seen = nullptr;
  fixture.RunOnGuestThread([&](rex::testing::Caller&) { seen = &rex::runtime::GetCurrent(); });
  CHECK(seen == &fixture.GetRuntime());
}

REX_TEST_CASE("Current returns the runtime inside an interrupt callback", "[runtime][current]") {
  rex::testing::KernelFixture fixture;
  g_seen_runtime = nullptr;
  fixture.RunOnGuestThread([](rex::testing::Caller& c) {
    auto* dispatcher = c.GetRuntime().GetFunctionDispatcher();
    const u32 callback = dispatcher->AllocateThunk(&RecordCurrent, 0);
    CHECKED_ELSE(callback != 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 pcr = c.Alloc(0x1000);
    const u32 stack = c.Alloc(0x10000) + 0x8000;
    std::jthread worker([&] {
      rex::runtime::ThreadState state(4, stack, pcr, c.GetRuntime().GetMemory(), &c.GetRuntime());
      dispatcher->ExecuteInterrupt(&state, callback, nullptr, 0);
    });
    worker.join();
  });
  CHECK(g_seen_runtime.load() == &fixture.GetRuntime());
}

REX_TEST_CASE("A host thread with no thread state launches a worker thread", "[runtime][current]") {
  rex::testing::KernelFixture fixture;
  std::atomic<bool> ran{false};
  bool launched = false;
  std::jthread host([&] {
    auto thread =
        std::make_unique<rex::runtime::KernelThread>(fixture.GetRuntime(), 64 * 1024, 0, [&] {
          ran = true;
          return 0;
        });
    launched = fixture.GetRuntime().GetThreadRegistry().Launch(std::move(thread)).has_value();
  });
  host.join();
  REQUIRE(launched);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!ran && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  CHECK(ran);
}

namespace {

void CheckServed(rex::API& api, rex::Runtime& runtime) {
  CHECK(api.Get<rex::runtime::IObjectTable>() == &runtime.GetObjectTable());
  CHECK(api.Get<rex::runtime::IThreadRegistry>() == &runtime.GetThreadRegistry());
  CHECK(api.Get<rex::runtime::IModuleRegistry>() == &runtime.GetModuleRegistry());
  CHECK(api.Get<rex::runtime::INotifier>() == &runtime.GetNotifier());
  CHECK(api.Get<rex::runtime::IContentManager>() == &runtime.GetContentManager());
  CHECK(api.Get<rex::runtime::IAchievementManager>() == &runtime.GetAchievementManager());
  CHECK(api.Get<rex::runtime::IDispatcher>() == &runtime.GetDispatcher());
}

void CheckNoneServed(rex::API& api) {
  CHECK(api.Get<rex::runtime::IObjectTable>() == nullptr);
  CHECK(api.Get<rex::runtime::IThreadRegistry>() == nullptr);
  CHECK(api.Get<rex::runtime::IModuleRegistry>() == nullptr);
  CHECK(api.Get<rex::runtime::INotifier>() == nullptr);
  CHECK(api.Get<rex::runtime::IContentManager>() == nullptr);
  CHECK(api.Get<rex::runtime::IAchievementManager>() == nullptr);
  CHECK(api.Get<rex::runtime::IDispatcher>() == nullptr);
}

}  // namespace

REX_TEST_CASE("The module serves the 7 interfaces of the active runtime", "[runtime][interfaces]") {
  rex::testing::KernelFixture fixture;
  CheckServed(fixture.GetAPI(), fixture.GetRuntime());
}

REX_TEST_CASE("The module serves no interface after Shutdown", "[runtime][interfaces]") {
  rex::testing::KernelFixture fixture;
  fixture.GetRuntime().Shutdown();
  CheckNoneServed(fixture.GetAPI());
}

REX_TEST_CASE("A second runtime in the same process is served", "[runtime][interfaces]") {
  { rex::testing::KernelFixture first; }
  rex::testing::KernelFixture second;
  CheckServed(second.GetAPI(), second.GetRuntime());
}

REX_TEST_CASE("Setup fails while another runtime is active", "[runtime][interfaces]") {
  rex::testing::KernelFixture fixture;
  rex::Runtime other(fixture.GetAPI().GetConfig());
  rex::RuntimeDesc config;
  config.tool_mode = true;
  CHECK_FALSE(other.Setup(config));
  CheckServed(fixture.GetAPI(), fixture.GetRuntime());
}

REX_TEST_CASE("process_type reads what set_process_type wrote", "[runtime][interfaces]") {
  rex::testing::KernelFixture fixture;
  fixture.GetRuntime().SetProcessType(rex::runtime::kXProcTypeSystem);
  CHECK(fixture.GetRuntime().GetProcessType() == rex::runtime::kXProcTypeSystem);
  fixture.GetRuntime().SetProcessType(rex::runtime::kXProcTypeUser);
  CHECK(fixture.GetRuntime().GetProcessType() == rex::runtime::kXProcTypeUser);
}

REX_TEST_CASE("ResolveHandle takes the guest address of a dispatch header",
              "[runtime][interfaces]") {
  rex::testing::KernelFixture fixture;
  fixture.RunOnGuestThread([](rex::testing::Caller& c) {
    const u32 address = c.Alloc(sizeof(rex::runtime::X_KEVENT));
    auto* event = c.GetRuntime().GetMemory()->TranslateVirtual<rex::runtime::X_KEVENT*>(address);
    std::memset(event, 0, sizeof(rex::runtime::X_KEVENT));
    auto& table = c.GetRuntime().GetObjectTable();
    const std::optional<rex::X_HANDLE> handle = table.ResolveHandle(address, -1);
    CHECKED_ELSE(handle.has_value()) {
      FAIL_CHECK("the check above failed");
      return;
    }
    CHECK(table.ResolveHandle(address, -1) == handle);
    CHECK(table.Resolve<rex::runtime::EventObject>(address));
  });
}

REX_TEST_CASE("ResolveHandle finds the newest object at a block after the older one is released",
              "[runtime][interfaces]") {
  rex::testing::KernelFixture fixture;
  fixture.RunOnGuestThread([](rex::testing::Caller& c) {
    const u32 address = c.Alloc(sizeof(rex::runtime::X_KEVENT));
    auto& table = c.GetRuntime().GetObjectTable();
    auto older = table.Emplace<rex::runtime::EventObject>(
        rex::runtime::AddOptions{{}, {.address = address}}, true, false);
    auto newer = table.Emplace<rex::runtime::EventObject>(
        rex::runtime::AddOptions{{}, {.address = address}}, true, false);
    REQUIRE(older);
    REQUIRE(newer);
    CHECK(table.ResolveHandle(address, -1) == *newer);
    REQUIRE(table.Close(*older));
    CHECK(table.ResolveHandle(address, -1) == *newer);
  });
}

REX_TEST_CASE("is_setup holds only between a successful Setup and Shutdown",
              "[runtime][interfaces]") {
  rex::testing::KernelFixture fixture;
  CHECK(fixture.GetRuntime().IsSetup());
  rex::Runtime other(fixture.GetAPI().GetConfig());
  rex::RuntimeDesc config;
  config.tool_mode = true;
  CHECK_FALSE(other.Setup(config));
  CHECK_FALSE(other.IsSetup());
  fixture.GetRuntime().Shutdown();
  CHECK_FALSE(fixture.GetRuntime().IsSetup());
}
