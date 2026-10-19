/**
 * @file        tests/unit/kernel/threads_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <thread>

#include <rex/platform/thread.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime/kernel_list.h>
#include <rex/runtime/thread_registry.h>
#include <rex/runtime/xmemory.h>
#include <rex/runtime/xthread.h>
#include <rex/runtime/xtypes.h>

#include "kernel_fixture.h"

using rex::u32;
using rex::u64;
using rex::X_STATUS;

using rex::test::Caller;
using rex::test::Join;
using rex::test::KernelFixture;

namespace {

u32 KThread(Caller& c) {
  return c.Load32(c.GetContext().r13.u32 + 0x100);
}

u32 SpawnWithStack(Caller& c, u32 stack_size, std::function<void(Caller&)> body, u32 flags = 0) {
  const u32 handle_ptr = c.Alloc(4);
  const u32 status = c.Kernel("ExCreateThread", {handle_ptr, stack_size, 0, 0, Caller::start_thunk,
                                                 Caller::RegisterBody(std::move(body)), flags});
  CHECKED_ELSE(status == 0) {
    FAIL_CHECK("the check above failed");
    return 0;
  }
  return c.Load32(handle_ptr);
}

u32 StackSizeOf(Caller& c, u32 stack_size) {
  auto size = std::make_shared<std::atomic<u32>>(0);
  const u32 handle = SpawnWithStack(c, stack_size, [size](Caller& t) {
    const u32 kthread = KThread(t);
    size->store(t.Load32(kthread + 0x5C) - t.Load32(kthread + 0x60));
  });
  Join(c, handle);
  return size->load();
}

bool WaitForTitlePages(rex::runtime::Memory& memory, u32 pages) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (memory.GetTitlePages() != pages && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return memory.GetTitlePages() == pages;
}

u32 ReferenceThreadObject(Caller& c, u32 handle) {
  const u32 out = c.Alloc(4);
  CHECKED_ELSE(c.Kernel("ObReferenceObjectByHandle", {handle, 0, out}) == 0) {
    FAIL_CHECK("the check above failed");
    return 0;
  }
  return c.Load32(out);
}

u64 Load64(Caller& c, u32 address) {
  return (static_cast<u64>(c.Load32(address)) << 32) | c.Load32(address + 4);
}

u32 ExitCodeXAPI(Caller& c, u32 kthread) {
  return c.Load32(kthread + 4) ? c.Load32(kthread + 0x140) : 0x103;
}

std::chrono::milliseconds SpinCPU(std::chrono::milliseconds duration) {
  const auto start = rex::platform::GetCurrentThreadCPUTime();
  const auto cap = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  volatile u64 counter = 0;
  while (rex::platform::GetCurrentThreadCPUTime() - start < duration &&
         std::chrono::steady_clock::now() < cap) {
    counter = counter + 1;
  }
  return rex::platform::GetCurrentThreadCPUTime() - start;
}

}  // namespace

REX_TEST_CASE("threads: create suspended resume and wait for exit", "[kernel][threads]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto ran = std::make_shared<std::atomic<u32>>(0);
    const u32 handle = c.Spawn([ran](Caller&) { ran->store(1); }, 1);
    CHECK(ran->load() == 0);
    const u32 count = c.Alloc(4);
    CHECK(c.Kernel("NtResumeThread", {handle, count}) == 0);
    CHECK(c.Load32(count) == 1);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, c.Quad(-50000000)}) == 0);
    CHECK(ran->load() == 1);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

REX_TEST_CASE("threads: suspend and resume counts", "[kernel][threads]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle = c.Spawn([](Caller&) {}, 1);
    const u32 count = c.Alloc(4);
    CHECK(c.Kernel("NtSuspendThread", {handle, count}) == 0);
    CHECK(c.Load32(count) == 1);
    CHECK(c.Kernel("NtResumeThread", {handle, count}) == 0);
    CHECK(c.Load32(count) == 2);
    CHECK(c.Kernel("NtResumeThread", {handle, count}) == 0);
    CHECK(c.Load32(count) == 1);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, c.Quad(-50000000)}) == 0);
    c.Kernel("NtClose", {handle});
  });
}

REX_TEST_CASE("threads: a handle outlives the thread", "[kernel][threads]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle = c.Spawn([](Caller&) {});
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, c.Quad(-50000000)}) == 0);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, c.Quad(0)}) == 0);
    const u32 copy_ptr = c.Alloc(4);
    CHECK(c.Kernel("NtDuplicateObject", {handle, copy_ptr, 0}) == 0);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {c.Load32(copy_ptr), 1, 0, c.Quad(0)}) == 0);
    CHECK(c.Kernel("NtClose", {c.Load32(copy_ptr)}) == 0);
  });
}

REX_TEST_CASE("threads: ObLookupThreadByThreadId finds a live thread and not a dead id",
              "[kernel][threads]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 id_ptr = c.Alloc(4);
    const u32 handle_ptr = c.Alloc(4);
    auto release = std::make_shared<std::promise<void>>();
    auto gate = release->get_future().share();
    CHECKED_ELSE(c.Kernel("ExCreateThread",
                          {handle_ptr, 0x10000, id_ptr, 0, Caller::start_thunk,
                           Caller::RegisterBody([gate](Caller&) { gate.wait(); }), 0}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 id = c.Load32(id_ptr);
    const u32 object_ptr = c.Alloc(4);
    CHECK(c.Kernel("ObLookupThreadByThreadId", {id, object_ptr}) == 0);
    CHECK(c.Load32(object_ptr) != 0);
    CHECK(c.Kernel("ObDereferenceObject", {c.Load32(object_ptr)}) == 0);
    release->set_value();
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {c.Load32(handle_ptr), 1, 0, c.Quad(-50000000)}) ==
          0);
    c.Kernel("NtClose", {c.Load32(handle_ptr)});
    CHECK(c.Kernel("ObLookupThreadByThreadId", {0x7FFFFFF0, object_ptr}) ==
          rex::X_STATUS_NOT_FOUND);
  });
}

REX_TEST_CASE("threads: ExTerminateThread ends the thread and signals its handle",
              "[kernel][threads]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle = c.Spawn([](Caller& o) { o.Kernel("ExTerminateThread", {7}); });
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, c.Quad(-50000000)}) == 0);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

REX_TEST_CASE("threads: a queued user APC runs during an alertable wait",
              "[kernel][threads][apc]") {
  static std::atomic<u32> g_apc_calls;
  g_apc_calls = 0;
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto* dispatcher = c.GetRuntime().GetFunctionDispatcher();
    const u32 routine = dispatcher->AllocateThunk(
        +[](PPCContext& ctx, u8*) {
          ++g_apc_calls;
          ctx.r3.u64 = 0;
        },
        0);
    auto wait_result = std::make_shared<std::promise<u32>>();
    auto done = wait_result->get_future();
    auto waiting = std::make_shared<std::atomic<bool>>(false);
    const u32 thread = c.Spawn([wait_result, waiting](Caller& o) {
      const u32 event_ptr = o.Alloc(4);
      o.Kernel("NtCreateEvent", {event_ptr, 0, 0, 0});
      waiting->store(true);
      wait_result->set_value(
          o.Kernel("NtWaitForSingleObjectEx", {o.Load32(event_ptr), 1, 1, o.Quad(-100000000)}));
    });
    while (!waiting->load()) {
      std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    c.Kernel("NtQueueApcThread", {thread, routine, 5, 6, 7});
    CHECK(done.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    CHECK(done.get() == rex::X_STATUS_USER_APC);
    CHECK(g_apc_calls == 1);
    c.Kernel("NtClose", {thread});
  });
}

REX_TEST_CASE("threads: a guest APC with a kernel routine is delivered without freeing its memory",
              "[kernel][threads][apc]") {
  static std::atomic<u32> g_sequence;
  static std::atomic<u32> g_kernel_at;
  static std::atomic<u32> g_normal_at;
  g_sequence = 0;
  g_kernel_at = 0;
  g_normal_at = 0;
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto* dispatcher = c.GetRuntime().GetFunctionDispatcher();
    const u32 kernel_routine = dispatcher->AllocateThunk(
        +[](PPCContext& ctx, u8*) {
          g_kernel_at = ++g_sequence;
          ctx.r3.u64 = 0;
        },
        0);
    const u32 normal_routine = dispatcher->AllocateThunk(
        +[](PPCContext& ctx, u8*) {
          g_normal_at = ++g_sequence;
          ctx.r3.u64 = 0;
        },
        0);
    const u32 apc = c.Alloc(rex::runtime::XAPC::kSize);
    c.Kernel("KeInitializeApc", {apc, KThread(c), kernel_routine, 0, normal_routine, 1, 0x1234});
    REQUIRE(c.Kernel("KeInsertQueueApc", {apc, 5, 6, 0}) == 1);
    c.Kernel("KeDelayExecutionThread", {1, 1, c.Quad(-10000)});

    CHECK(g_kernel_at == 1);
    CHECK(g_normal_at == 2);
    rex::runtime::HeapAllocation info{};
    REQUIRE(c.GetRuntime().GetMemory()->LookupHeap(apc)->QueryRegionInfo(apc, &info));
    CHECK((info.state & rex::runtime::kMemoryAllocationCommit) != 0);
  });
}

REX_TEST_CASE("threads: KeDelayExecutionThread holds a sub millisecond interval to its tick count",
              "[kernel][threads][delay]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    using Clock = std::chrono::steady_clock;
    INFO("clock source std::chrono::steady_clock, period " << Clock::period::num << "/"
                                                           << Clock::period::den << " s");
    for (const u32 alertable : {0u, 1u}) {
      for (const rex::i64 ticks : {1, 5000, 15000}) {
        const u32 interval = c.Quad(-ticks);
        const auto requested = std::chrono::nanoseconds(ticks * 100);
        std::array<std::chrono::nanoseconds, 21> samples;
        for (auto& sample : samples) {
          const auto start = Clock::now();
          c.Kernel("KeDelayExecutionThread", {1, alertable, interval});
          sample = Clock::now() - start;
          CAPTURE(alertable, ticks, requested.count(), sample.count());
          CHECK(sample >= requested);
        }
        std::ranges::sort(samples);
        if (ticks == 1) {
          const auto median = samples[samples.size() / 2];
          CAPTURE(alertable, requested.count(), median.count());
          CHECK(median < std::chrono::microseconds(500));
        }
      }
    }
  });
}

REX_TEST_CASE("threads: an APC queued before a non-alertable wait runs at the next alertable wait",
              "[kernel][threads][apc]") {
  static std::atomic<u32> g_apc_calls;
  g_apc_calls = 0;
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 routine = c.GetRuntime().GetFunctionDispatcher()->AllocateThunk(
        +[](PPCContext& ctx, u8*) {
          ++g_apc_calls;
          ctx.r3.u64 = 0;
        },
        0);
    const u32 events = c.Alloc(8);
    REQUIRE(c.Kernel("NtCreateEvent", {events, 0, 0, 1}) == 0);
    REQUIRE(c.Kernel("NtCreateEvent", {events + 4, 0, 0, 0}) == 0);
    const u32 signaled = c.Load32(events);
    const u32 idle = c.Load32(events + 4);
    const auto queue_behind_plain_wait = [&] {
      rex::runtime::KernelThread::GetCurrentThread()->EnqueueAPC(routine, 0, 0, 0);
      CHECK(c.Kernel("NtWaitForSingleObjectEx", {signaled, 1, 0, c.Quad(0)}) == 0);
    };
    const u32 calls_before = g_apc_calls;

    queue_behind_plain_wait();
    CHECK(g_apc_calls == calls_before);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {idle, 1, 1, c.Quad(-20000000)}) ==
          rex::X_STATUS_USER_APC);
    CHECK(g_apc_calls == calls_before + 1);

    queue_behind_plain_wait();
    CHECK(c.Kernel("NtWaitForMultipleObjectsEx", {1, events + 4, 1, 1, 1, c.Quad(-20000000)}) ==
          rex::X_STATUS_USER_APC);
    CHECK(g_apc_calls == calls_before + 2);

    queue_behind_plain_wait();
    CHECK(c.Kernel("NtSignalAndWaitForSingleObjectEx", {signaled, idle, 1, 0, c.Quad(-20000000)}) ==
          rex::X_STATUS_USER_APC);
    CHECK(g_apc_calls == calls_before + 3);

    queue_behind_plain_wait();
    CHECK(c.Kernel("KeDelayExecutionThread", {1, 1, c.Quad(-20000000)}) == rex::X_STATUS_USER_APC);
    CHECK(g_apc_calls == calls_before + 4);

    c.Kernel("NtClose", {signaled});
    c.Kernel("NtClose", {idle});
  });
}

REX_TEST_CASE("threads: title termination ends a thread that suspends itself",
              "[kernel][threads][terminate]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto handle = std::make_shared<std::atomic<u32>>(0);
    auto suspending = std::make_shared<std::atomic<bool>>(false);
    handle->store(c.Spawn([handle, suspending](Caller& o) {
      while (handle->load() == 0) {
        std::this_thread::yield();
      }
      for (;;) {
        suspending->store(true);
        o.Kernel("NtSuspendThread", {handle->load(), 0});
      }
    }));
    while (!suspending->load()) {
      std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  });
  const auto start = std::chrono::steady_clock::now();
  fixture.GetRuntime().TerminateTitle();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
  CHECK(elapsed < 1500);
}

REX_TEST_CASE("threads: a stack of size 0 takes the process default", "[kernel][threads][stack]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 process = c.GetRuntime().GetTitleProcess();
    CHECK(c.Load32(process + 0x1C) == 0x10000);
    CHECK(StackSizeOf(c, 0) == 0x10000);
    c.Store32(process + 0x1C, 0x6000);
    CHECK(StackSizeOf(c, 0) == 0x6000);
    CHECK(StackSizeOf(c, 0x5001) == 0x6000);
    CHECK(StackSizeOf(c, 0x1000) == 0x4000);
    c.Store32(process + 0x1C, 0x10000);
  });
}

REX_TEST_CASE("threads: the guard page is invalid and the TLS block starts at the stack base",
              "[kernel][threads][stack]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 kthread = KThread(c);
    const u32 base = c.Load32(kthread + 0x5C);
    const u32 limit = c.Load32(kthread + 0x60);
    const u32 guard = (limit & ~0xFFFFu) - 0x10000;
    CHECK(c.Kernel("MmIsAddressValid", {guard}) == 0);
    CHECK(c.Kernel("MmIsAddressValid", {guard + 0xFFFF}) == 0);
    CHECK(c.Kernel("MmIsAddressValid", {limit}) == 1);
    CHECK(c.Kernel("MmIsAddressValid", {base - 4}) == 1);
    CHECK(c.Load32(c.GetContext().r13.u32) == base);
    CHECK(c.Load32(kthread + 0x68) == base);
  });
}

REX_TEST_CASE("threads: a guest stack counts against the budget while the thread runs",
              "[kernel][threads][stack]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto* memory = c.GetRuntime().GetMemory();
    const u32 before = memory->GetTitlePages();
    auto used = std::make_shared<std::atomic<u32>>(0);
    const u32 handle = SpawnWithStack(
        c, 0x10000, [used, memory](Caller&) { used->store(memory->GetTitlePages()); });
    Join(c, handle);
    CHECK(used->load() - before == 32);
    CHECK(WaitForTitlePages(*memory, before));
  });
}

REX_TEST_CASE("threads: ExCreateThread past the budget returns X_STATUS_NO_MEMORY",
              "[kernel][threads][stack]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto* memory = c.GetRuntime().GetMemory();
    memory->SetBudget(memory->GetTitlePages() / 256 + 1);
    const u32 room = memory->GetBudgetPages() - memory->GetTitlePages();
    u32 filler = 0;
    if (room > 8) {
      CHECKED_ELSE(memory->LookupHeap(0x10000000)
                       ->Alloc((room - 8) * 4096, 4096,
                               rex::runtime::kMemoryAllocationReserve |
                                   rex::runtime::kMemoryAllocationCommit |
                                   rex::runtime::kMemoryAllocationTitle,
                               rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite,
                               false, &filler)) {
        FAIL_CHECK("the check above failed");
        return;
      }
    }
    const u32 handle_ptr = c.Alloc(4);
    CHECK(c.Kernel("ExCreateThread", {handle_ptr, 0x10000, 0, 0, Caller::start_thunk,
                                      Caller::RegisterBody([](Caller&) {}), 0}) == 0xC0000017);
    if (filler) {
      memory->LookupHeap(0x10000000)->Release(filler);
    }
    memory->SetBudget(480);
  });
}

REX_TEST_CASE(
    "threads: MmCreateKernelStack returns a counted stack and MmDeleteKernelStack frees it",
    "[kernel][threads][stack]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto* memory = c.GetRuntime().GetMemory();
    const u32 before = memory->GetTitlePages();
    const u32 base = c.Kernel("MmCreateKernelStack", {0x4000, 0});
    CHECKED_ELSE(base != 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    CHECK(c.Kernel("MmIsAddressValid", {base - 0x4000}) == 1);
    CHECK(memory->GetTitlePages() == before + 16);
    CHECK(c.Kernel("MmDeleteKernelStack", {base, base - 0x4000}) == 0);
    CHECK(memory->GetTitlePages() == before);
  });
}

REX_TEST_CASE("threads: MmDeleteKernelStack refuses a base that is not the top of a kernel stack",
              "[kernel][threads][stack]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 base = c.Kernel("MmCreateKernelStack", {0x4000, 0});
    CHECKED_ELSE(base != 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    CHECK(c.Kernel("MmDeleteKernelStack", {base - 0x1000, base - 0x4000}) ==
          rex::X_STATUS_INVALID_PARAMETER);
    CHECK(c.Kernel("MmDeleteKernelStack", {base, base}) == rex::X_STATUS_INVALID_PARAMETER);
    CHECK(c.Kernel("MmIsAddressValid", {base - 0x4000}) == 1);
    CHECK(c.Kernel("MmDeleteKernelStack", {base, base - 0x4000}) == 0);
  });
}

REX_TEST_CASE("threads: exit writes the exit code, state 4, the exit time and the process count",
              "[kernel][threads][exit]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 process = c.GetRuntime().GetTitleProcess();
    const u32 count = c.Load32(process + 0x14);
    auto stage = std::make_shared<std::atomic<int>>(0);
    const u32 handle = c.Spawn([stage](Caller& t) {
      while (stage->load() == 0) {
        std::this_thread::yield();
      }
      t.Kernel("ExTerminateThread", {3});
    });
    const u32 kthread = ReferenceThreadObject(c, handle);
    CHECK(ExitCodeXAPI(c, kthread) == 0x103);
    CHECK(c.Load32(process + 0x14) == count + 1);
    stage->store(1);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, c.Quad(-50000000)}) == 0);
    CHECK(ExitCodeXAPI(c, kthread) == 3);
    CHECK(c.Load8(kthread + 0x6C) == 4);
    CHECK(c.Load8(kthread + 0xBE) == 1);
    CHECK(Load64(c, kthread + 0x138) >= Load64(c, kthread + 0x130));
    CHECK(Load64(c, kthread + 0x138) != 0);
    CHECK(c.Load32(process + 0x14) == count);
    c.Kernel("ObDereferenceObject", {kthread});
    c.Kernel("NtClose", {handle});
  });
}

REX_TEST_CASE("threads: a stop writes the same exit state before a waiter wakes",
              "[kernel][threads][exit]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 process = c.GetRuntime().GetTitleProcess();
    const u32 handle = c.Spawn([](Caller&) {}, 1);
    const u32 kthread = ReferenceThreadObject(c, handle);
    const u32 count = c.Load32(process + 0x14);
    auto seen = std::make_shared<std::atomic<u32>>(0);
    const u32 waiter = c.Spawn([handle, kthread, seen](Caller& w) {
      w.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, w.Quad(-50000000)});
      seen->store(ExitCodeXAPI(w, kthread));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    {
      auto pin = c.GetRuntime().GetThreadRegistry().FindByHandle(handle);
      CHECKED_ELSE(pin) {
        FAIL_CHECK("the check above failed");
        return;
      }
      pin->RequestStop(9);
    }
    c.Kernel("NtResumeThread", {handle, 0});
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {waiter, 1, 0, c.Quad(-50000000)}) == 0);
    c.Kernel("NtClose", {waiter});
    CHECK(seen->load() == 9);
    CHECK(ExitCodeXAPI(c, kthread) == 9);
    CHECK(c.Load8(kthread + 0x6C) == 4);
    CHECK(c.Load8(kthread + 0xBE) == 1);
    CHECK(Load64(c, kthread + 0x138) != 0);
    CHECK(c.Load32(process + 0x14) == count - 1);
    c.Kernel("ObDereferenceObject", {kthread});
    c.Kernel("NtClose", {handle});
  });
}

REX_TEST_CASE("threads: kernel time grows across a busy loop and a wait",
              "[kernel][threads][times]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 kthread = KThread(c);
    c.Kernel("NtYieldExecution");
    const u32 first = c.Load32(kthread + 0x58);
    const auto spun = SpinCPU(std::chrono::milliseconds(150));
    CHECK(spun >= std::chrono::milliseconds(150));
    const u32 event_ptr = c.Alloc(4);
    CHECKED_ELSE(c.Kernel("NtCreateEvent", {event_ptr, 0, 0, 0}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 event = c.Load32(event_ptr);
    c.Kernel("NtWaitForSingleObjectEx", {event, 1, 0, c.Quad(-10000)});
    INFO("host thread CPU time " << spun.count() << " ms");
    CHECK(c.Load32(kthread + 0x58) - first >= 100);
    c.Kernel("NtClose", {event});
  });
}

REX_TEST_CASE("threads: a new thread starts on its creator's processor", "[kernel][threads][cpu]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    CHECK(c.Load8(c.GetContext().r13.u32 + 0x10C) == 0);
    auto parent = std::make_shared<std::atomic<u32>>(0xFF);
    auto child = std::make_shared<std::atomic<u32>>(0xFF);
    const u32 handle = c.Spawn(
        [parent, child](Caller& t) {
          parent->store(t.Load8(t.GetContext().r13.u32 + 0x10C));
          const u32 inner = t.Spawn(
              [child](Caller& u) { child->store(u.Load8(u.GetContext().r13.u32 + 0x10C)); });
          t.Kernel("NtWaitForSingleObjectEx", {inner, 1, 0, t.Quad(-50000000)});
          t.Kernel("NtClose", {inner});
        },
        0x10000000);
    Join(c, handle);
    CHECK(parent->load() == 4);
    CHECK(child->load() == 4);
  });
}

REX_TEST_CASE("A new thread links its wait, timer and spare lists to themselves",
              "[kernel][threads]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto* object =
        rex::runtime::KernelThread::GetCurrentThread()->GetGuestObject<rex::runtime::X_KTHREAD>();
    auto* memory = c.GetRuntime().GetMemory();
    for (auto* list :
         {&object->wait_timeout_block.wait_list_entry, &object->timer_list, &object->unk_154}) {
      const rex::u32 address = rex::runtime::GuestListAddress(list, memory);
      CHECK(list->flink_ptr == address);
      CHECK(list->blink_ptr == address);
    }
  });
}

namespace {

u32 CPUOfThreadCreatedWith(Caller& c, u32 creation_flags) {
  auto cpu = std::make_shared<std::atomic<u32>>(0xFF);
  const u32 handle = c.Spawn(
      [cpu](Caller&) {
        cpu->store(rex::runtime::KernelThread::GetCurrentThread()->GetActiveCPU());
      },
      creation_flags);
  Join(c, handle);
  return cpu->load();
}

u32 CPUAfterSetAffinity(Caller& c, u32 affinity) {
  auto* thread = rex::runtime::KernelThread::GetCurrentThread();
  const u32 object = thread->GetGuestObject();
  c.Kernel("KeSetAffinityThread", {object, affinity, 0});
  return thread->GetActiveCPU();
}

}  // namespace

REX_TEST_CASE("A creation mask without a valid processor bit inherits the creator processor",
              "[kernel][threads][affinity]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 creator = rex::runtime::KernelThread::GetCurrentThread()->GetActiveCPU();
    CHECK(CPUOfThreadCreatedWith(c, 0xC0000000) == creator);
    CHECK(CPUOfThreadCreatedWith(c, 0x80000000) == creator);
  });
}

REX_TEST_CASE("A creation mask with several bits picks a processor from the mask",
              "[kernel][threads][affinity]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    CHECK(CPUOfThreadCreatedWith(c, 0x28000000) == 3);
    CHECK(CPUOfThreadCreatedWith(c, 0xFF000000) < 6);
    CHECK(CPUOfThreadCreatedWith(c, 0x3F000000) < 6);
    CHECK(CPUOfThreadCreatedWith(c, 0x60000000) == 5);
  });
}

REX_TEST_CASE("KeSetAffinityThread keeps the processor inside the 6 hardware threads",
              "[kernel][threads][affinity]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    CHECK(CPUAfterSetAffinity(c, 0x04) == 2);
    CHECK(CPUAfterSetAffinity(c, 0xC0) == 2);
    CHECK(CPUAfterSetAffinity(c, 0x00) == 2);
    CHECK(c.Kernel("KeSetAffinityThread",
                   {rex::runtime::KernelThread::GetCurrentThread()->GetGuestObject(), 0xC0, 0}) ==
          0);
    CHECK(CPUAfterSetAffinity(c, 0x3F) == 2);
    CHECK(CPUAfterSetAffinity(c, 0x30) == 4);
    CHECK(CPUAfterSetAffinity(c, 0xFF) == 4);
    CHECK(CPUAfterSetAffinity(c, 0x40) == 4);
  });
}

namespace {

std::string NameAfterSetThreadNameRecord(Caller& c, u32 type, u32 name) {
  const u32 record = c.Alloc(0x50);
  c.Store32(record, 0x406D1388);
  c.Store32(record + 16, 4);
  c.Store32(record + 20, type);
  c.Store32(record + 24, name);
  c.Store32(record + 28, 0xFFFFFFFF);
  c.Kernel("RtlRaiseException", {record});
  return rex::runtime::KernelThread::GetCurrentThread()->GetName();
}

}  // namespace

REX_TEST_CASE("A thread name record of another type leaves the thread name alone",
              "[kernel][threads][name]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 name = c.String("Worker");
    const std::string before = rex::runtime::KernelThread::GetCurrentThread()->GetName();
    CHECK(NameAfterSetThreadNameRecord(c, 0x2000, name) == before);
    CHECK(NameAfterSetThreadNameRecord(c, 0x1000, name).rfind("Worker", 0) == 0);
  });
}

REX_TEST_CASE("A thread name without a terminator is cut at a fixed length",
              "[kernel][threads][name]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 name = c.String(std::string(2000, 'A'));
    const std::string applied = NameAfterSetThreadNameRecord(c, 0x1000, name);
    CHECK(applied.rfind(std::string(256, 'A') + " (", 0) == 0);
  });
}
