/**
 * @file        tests/unit/kernel/runtime_interfaces_test.cpp
 * @brief       Runtime lookup from guest and host threads
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

#include <rex/kernel/title_runtime.h>
#include <rex/kernel/thread_registry.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/kernel/module.h>
#include <rex/kernel/object/event_object.h>
#include <rex/kernel/object/object_table.h>
#include <rex/kernel/types/thread.h>
#include <rex/kernel/thread_state.h>
#include <rex/kernel/object/kernel_thread.h>

#include "../kernel/kernel_fixture.h"
#include "kernel/title_state.h"

using rex::kernel::X_STATUS;

namespace {

std::atomic<rex::kernel::TitleRuntime*> g_seen_runtime{nullptr};

void CaptureCurrent(PPCContext& ctx, u8*) {
  g_seen_runtime = &rex::kernel::GetCurrent();
  ctx.r3.u64 = 0;
}

}  // namespace

REX_TEST_CASE("Current returns the runtime on a guest thread", "[kernel][current]") {
  rex::test::KernelFixture fixture;
  rex::kernel::TitleRuntime* seen = nullptr;
  fixture.RunOnGuestThread([&](rex::test::Caller&) { seen = &rex::kernel::GetCurrent(); });
  CHECK(seen == &fixture.GetRuntime());
}

REX_TEST_CASE("Current returns the runtime inside an interrupt callback", "[kernel][current]") {
  rex::test::KernelFixture fixture;
  g_seen_runtime = nullptr;
  fixture.RunOnGuestThread([](rex::test::Caller& c) {
    auto* dispatcher = StateOf(c.GetRuntime()).GetFunctionDispatcher();
    const u32 callback = dispatcher->AllocateThunk(&CaptureCurrent, 0);
    CHECKED_ELSE(callback != 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 pcr = c.Alloc(0x1000);
    const u32 stack = c.Alloc(0x10000) + 0x8000;
    std::jthread worker([&] {
      rex::kernel::ThreadState state(4, stack, pcr, StateOf(c.GetRuntime()).GetAddressSpace(),
                                     &c.GetRuntime());
      dispatcher->ExecuteInterrupt(&state, callback, nullptr, 0);
    });
    worker.join();
  });
  CHECK(g_seen_runtime.load() == &fixture.GetRuntime());
}

REX_TEST_CASE("A host thread with no thread state launches a worker thread", "[kernel][current]") {
  rex::test::KernelFixture fixture;
  std::atomic<bool> ran{false};
  bool launched = false;
  std::jthread host([&] {
    auto thread =
        std::make_unique<rex::kernel::KernelThread>(fixture.GetRuntime(), 64 * 1024, 0, [&] {
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

REX_TEST_CASE("A second runtime in the same process loads", "[kernel][interfaces]") {
  { rex::test::KernelFixture first; }
  rex::test::KernelFixture second;
  CHECK(second.GetRuntime().IsLoaded());
}

REX_TEST_CASE("GetProcessType reads what SetProcessType wrote", "[kernel][interfaces]") {
  rex::test::KernelFixture fixture;
  StateOf(fixture.GetRuntime()).SetProcessType(rex::kernel::kXProcTypeSystem);
  CHECK(StateOf(fixture.GetRuntime()).GetProcessType() == rex::kernel::kXProcTypeSystem);
  StateOf(fixture.GetRuntime()).SetProcessType(rex::kernel::kXProcTypeUser);
  CHECK(StateOf(fixture.GetRuntime()).GetProcessType() == rex::kernel::kXProcTypeUser);
}

REX_TEST_CASE("ResolveObject takes the guest address of a dispatch header",
              "[kernel][interfaces]") {
  rex::test::KernelFixture fixture;
  fixture.RunOnGuestThread([](rex::test::Caller& c) {
    const u32 address = c.Alloc(sizeof(rex::kernel::X_KEVENT));
    auto* event = StateOf(c.GetRuntime())
                      .GetAddressSpace()
                      ->TranslateVirtual<rex::kernel::X_KEVENT*>(address);
    std::memset(event, 0, sizeof(rex::kernel::X_KEVENT));
    auto& table = c.GetRuntime().GetObjectTable();
    const rex::kernel::ObjectPin first = table.ResolveObject(address, -1);
    CHECKED_ELSE(static_cast<bool>(first)) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const rex::kernel::ObjectPin second = table.ResolveObject(address, -1);
    REQUIRE(second);
    CHECK(&second.GetKernelObject() == &first.GetKernelObject());
    CHECK(first.GetKernelObject().handles.empty());
    CHECK(table.Resolve<rex::kernel::EventObject>(address));
  });
}

REX_TEST_CASE("ResolveObject finds the newest object at a block after the older one is released",
              "[kernel][interfaces]") {
  rex::test::KernelFixture fixture;
  fixture.RunOnGuestThread([](rex::test::Caller& c) {
    const u32 address = c.Alloc(sizeof(rex::kernel::X_KEVENT));
    auto& table = c.GetRuntime().GetObjectTable();
    auto older = table.Emplace<rex::kernel::EventObject>(
        rex::kernel::AddOptions{{}, {.address = address}}, true, false);
    auto newer = table.Emplace<rex::kernel::EventObject>(
        rex::kernel::AddOptions{{}, {.address = address}}, true, false);
    REQUIRE(older);
    REQUIRE(newer);
    const rex::kernel::ObjectPin expected = table.Get(*newer);
    REQUIRE(expected);
    {
      const rex::kernel::ObjectPin found = table.ResolveObject(address, -1);
      REQUIRE(found);
      CHECK(&found.GetKernelObject() == &expected.GetKernelObject());
    }
    REQUIRE(table.Close(*older));
    const rex::kernel::ObjectPin found = table.ResolveObject(address, -1);
    REQUIRE(found);
    CHECK(&found.GetKernelObject() == &expected.GetKernelObject());
  });
}
