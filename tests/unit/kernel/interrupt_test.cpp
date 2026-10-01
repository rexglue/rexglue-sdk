#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime.h>
#include <rex/runtime/thread_state.h>

#include "kernel_fixture.h"

using rex::testing::Caller;
using rex::testing::KernelFixture;

namespace {

std::atomic<int> g_active;
std::atomic<int> g_max;

void RecordOverlap(PPCContext& ctx, u8*) {
  const int now = ++g_active;
  int expected = g_max.load();
  while (now > expected && !g_max.compare_exchange_weak(expected, now)) {}
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  --g_active;
  ctx.r3.u64 = 0;
}

}  // namespace

TEST_CASE("interrupt callbacks never overlap and never block guest threads", "[interrupt]") {
  g_active = 0;
  g_max = 0;
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto* dispatcher = c.runtime().function_dispatcher();
    auto* memory = c.runtime().memory();
    const u32 callback = dispatcher->AllocateThunk(&RecordOverlap, 0);
    REQUIRE(callback != 0);
    const u32 pcr_first = c.Alloc(0x1000);
    const u32 pcr_second = c.Alloc(0x1000);
    const u32 stack_first = c.Alloc(0x10000) + 0x8000;
    const u32 stack_second = c.Alloc(0x10000) + 0x8000;
    std::thread first([&] {
      rex::runtime::ThreadState state(1, stack_first, pcr_first, memory, &c.runtime());
      dispatcher->ExecuteInterrupt(&state, callback, nullptr, 0);
    });
    std::thread second([&] {
      rex::runtime::ThreadState state(2, stack_second, pcr_second, memory, &c.runtime());
      dispatcher->ExecuteInterrupt(&state, callback, nullptr, 0);
    });
    for (int i = 0; i < 1000; ++i) {
      const u32 old_irql = c.Kernel("KeRaiseIrqlToDpcLevel");
      c.Kernel("KfLowerIrql", {old_irql});
    }
    first.join();
    second.join();
  });
  CHECK(g_max == 1);
}

TEST_CASE("interrupts leave the guest thread state intact", "[interrupt]") {
  g_active = 0;
  g_max = 0;
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto* dispatcher = c.runtime().function_dispatcher();
    const u32 callback = dispatcher->AllocateThunk(&RecordOverlap, 0);
    REQUIRE(callback != 0);
    const u32 pcr = c.Alloc(0x1000);
    c.Store32(pcr, 0x1234);
    rex::runtime::ThreadState state(3, c.Alloc(0x10000) + 0x8000, pcr, c.runtime().memory(),
                                    &c.runtime());
    CHECK(dispatcher->ExecuteInterrupt(&state, callback, nullptr, 0) == 0);
    CHECK(c.Load32(pcr) == 0x1234);
  });
  CHECK(g_max == 1);
}

TEST_CASE("DPC init and removal from 2 guest threads leaves the list empty", "[interrupt]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto worker = [](Caller& o) {
      const u32 dpc = o.Alloc(0x40);
      for (int i = 0; i < 200; ++i) {
        o.Kernel("KeInitializeDpc", {dpc, 0x82000000, 0});
        o.Kernel("KeRemoveQueueDpc", {dpc});
      }
    };
    const u32 first = c.Spawn(worker);
    const u32 second = c.Spawn(worker);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {first, 1, 0, c.Quad(-100000000)}) == 0);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {second, 1, 0, c.Quad(-100000000)}) == 0);
    c.Kernel("NtClose", {first});
    c.Kernel("NtClose", {second});
    CHECK_FALSE(c.runtime().dispatcher().dpc_list().HasPending());
  });
}
