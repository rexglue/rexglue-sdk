/**
 * @file        tests/unit/kernel/wait_spin_test.cpp
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
#include <latch>
#include <memory>
#include <thread>
#include <vector>

#include <rex/kernel/thread_registry.h>
#include <rex/kernel/title_runtime.h>

#include "kernel_fixture.h"
#include "kernel/thread_wait.h"
#include "kernel/title_state.h"

using rex::i32;
using rex::u32;
using rex::test::Caller;
using rex::test::Join;
using rex::test::KernelFixture;

namespace {

constexpr u32 kLockCount = 16;
constexpr u32 kRecursionCount = 20;
constexpr u32 kOwningThread = 24;
constexpr std::chrono::nanoseconds kLongSpin = std::chrono::milliseconds(200);

u32 KThread(Caller& c) {
  return c.Load32(c.GetContext().r13.u32 + 0x100);
}

/** Sets the spin bound of guest waits for a scope and restores it after. */
class SpinBoundScope {
 public:
  explicit SpinBoundScope(std::chrono::nanoseconds bound)
      : saved_(rex::kernel::GetWaitSpinBound()) {
    rex::kernel::SetWaitSpinBoundForTest(bound);
  }
  ~SpinBoundScope() { rex::kernel::SetWaitSpinBoundForTest(saved_); }

  SpinBoundScope(const SpinBoundScope&) = delete;
  SpinBoundScope& operator=(const SpinBoundScope&) = delete;

 private:
  std::chrono::nanoseconds saved_;
};

/** Waits until the predicate holds or 5 seconds pass, and returns the predicate. */
template <typename Predicate>
bool WaitUntil(Predicate predicate) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!predicate() && std::chrono::steady_clock::now() < until) {
    std::this_thread::yield();
  }
  return predicate();
}

struct Fields {
  i32 lock_count;
  i32 recursion_count;
  u32 owning_thread;
};

Fields ReadFields(Caller& c, u32 section) {
  return {static_cast<i32>(c.LoadAcquire32(section + kLockCount)),
          static_cast<i32>(c.Load32(section + kRecursionCount)), c.Load32(section + kOwningThread)};
}

}  // namespace

REX_TEST_CASE("A contended critical section reaches every waiter under a nonstop owner",
              "[kernel][waits]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr int kWaiters = 3;
    constexpr int kAcquisitions = 200;
    const u32 section = c.Alloc(28);
    c.Kernel("RtlInitializeCriticalSection", {section});
    auto stop = std::make_shared<std::atomic<bool>>(false);
    auto inside = std::make_shared<std::atomic<u32>>(0);
    auto overlaps = std::make_shared<std::atomic<u32>>(0);
    auto foreign_owner = std::make_shared<std::atomic<u32>>(0);
    auto owner_rounds = std::make_shared<std::atomic<u32>>(0);
    auto counts = std::make_shared<std::array<std::atomic<u32>, kWaiters>>();
    const auto hold = [section, inside, overlaps, foreign_owner](Caller& t) {
      t.Kernel("RtlEnterCriticalSection", {section});
      if (inside->fetch_add(1) != 0) {
        overlaps->fetch_add(1);
      }
      if (t.Load32(section + kOwningThread) != KThread(t)) {
        foreign_owner->fetch_add(1);
      }
      inside->fetch_sub(1);
      t.Kernel("RtlLeaveCriticalSection", {section});
    };
    const u32 owner = c.Spawn([stop, owner_rounds, hold](Caller& t) {
      while (!stop->load()) {
        hold(t);
        owner_rounds->fetch_add(1);
      }
    });
    std::vector<u32> waiters;
    waiters.reserve(kWaiters);
    for (int i = 0; i < kWaiters; ++i) {
      waiters.push_back(c.Spawn([i, counts, hold](Caller& t) {
        for (int n = 0; n < kAcquisitions; ++n) {
          hold(t);
          (*counts)[i].fetch_add(1);
        }
      }));
    }
    for (const u32 waiter : waiters) {
      Join(c, waiter);
    }
    stop->store(true);
    Join(c, owner);
    for (int i = 0; i < kWaiters; ++i) {
      CAPTURE(i);
      CHECK((*counts)[i].load() == kAcquisitions);
    }
    CHECK(owner_rounds->load() > 0);
    CHECK(overlaps->load() == 0);
    CHECK(foreign_owner->load() == 0);
    const Fields after = ReadFields(c, section);
    CHECK(after.lock_count == -1);
    CHECK(after.recursion_count == 0);
    CHECK(after.owning_thread == 0);
  });
}

REX_TEST_CASE("Critical section fields follow the console sequence for 1, 2 and 4 threads",
              "[kernel][waits]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const std::chrono::nanoseconds measured = rex::kernel::GetWaitSpinBound();
    for (const std::chrono::nanoseconds bound : {measured, kLongSpin}) {
      SpinBoundScope scope(bound);
      for (const int threads : {1, 2, 4}) {
        CAPTURE(bound.count(), threads);
        const u32 section = c.Alloc(28);
        c.Kernel("RtlInitializeCriticalSection", {section});
        const u32 header = c.Load32(section);
        const u32 header_state = c.Load32(section + 4);
        const u32 self = KThread(c);

        c.Kernel("RtlEnterCriticalSection", {section});
        Fields step = ReadFields(c, section);
        CHECK(step.lock_count == 0);
        CHECK(step.recursion_count == 1);
        CHECK(step.owning_thread == self);
        c.Kernel("RtlEnterCriticalSection", {section});
        step = ReadFields(c, section);
        CHECK(step.lock_count == 1);
        CHECK(step.recursion_count == 2);
        c.Kernel("RtlLeaveCriticalSection", {section});
        step = ReadFields(c, section);
        CHECK(step.lock_count == 0);
        CHECK(step.recursion_count == 1);
        CHECK(step.owning_thread == self);

        const int waiters = threads - 1;
        auto seen = std::make_shared<std::vector<Fields>>();
        auto owners = std::make_shared<std::vector<u32>>();
        std::vector<u32> handles;
        handles.reserve(static_cast<size_t>(waiters));
        for (int i = 0; i < waiters; ++i) {
          handles.push_back(c.Spawn([section, seen, owners](Caller& t) {
            t.Kernel("RtlEnterCriticalSection", {section});
            seen->push_back(ReadFields(t, section));
            owners->push_back(KThread(t));
            t.Kernel("RtlLeaveCriticalSection", {section});
          }));
        }
        CHECK(WaitUntil([&] { return ReadFields(c, section).lock_count == waiters; }));
        step = ReadFields(c, section);
        CHECK(step.recursion_count == 1);
        CHECK(step.owning_thread == self);
        c.Kernel("RtlLeaveCriticalSection", {section});
        for (const u32 handle : handles) {
          Join(c, handle);
        }
        CHECKED_ELSE(seen->size() == static_cast<size_t>(waiters)) {
          FAIL_CHECK("the check above failed");
          return;
        }
        for (int k = 0; k < waiters; ++k) {
          CAPTURE(k);
          CHECK((*seen)[k].lock_count == waiters - 1 - k);
          CHECK((*seen)[k].recursion_count == 1);
          CHECK((*seen)[k].owning_thread == (*owners)[k]);
        }
        step = ReadFields(c, section);
        CHECK(step.lock_count == -1);
        CHECK(step.recursion_count == 0);
        CHECK(step.owning_thread == 0);
        CHECK(c.Load32(section) == header);
        CHECK(c.Load32(section + 4) == header_state);
      }
    }
  });
}

REX_TEST_CASE("A waiter asleep on a critical section is stopped by a thread stop",
              "[kernel][waits][threads]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    SpinBoundScope scope(std::chrono::nanoseconds(0));
    const u32 section = c.Alloc(28);
    c.Kernel("RtlInitializeCriticalSection", {section});
    c.Kernel("RtlEnterCriticalSection", {section});
    auto entered = std::make_shared<std::atomic<bool>>(false);
    const u32 waiter = c.Spawn([section, entered](Caller& t) {
      t.Kernel("RtlEnterCriticalSection", {section});
      entered->store(true);
    });
    CHECK(WaitUntil([&] { return ReadFields(c, section).lock_count == 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    {
      auto pin = c.GetRuntime().GetThreadRegistry().FindByHandle(waiter);
      CHECKED_ELSE(pin) {
        FAIL_CHECK("the check above failed");
        return;
      }
      pin->RequestStop(9);
    }
    Join(c, waiter);
    CHECK_FALSE(entered->load());
    const Fields after = ReadFields(c, section);
    CHECK(after.recursion_count == 1);
    CHECK(after.owning_thread == KThread(c));
  });
}

REX_TEST_CASE("A permit published while a waiter goes to sleep is taken", "[kernel][waits]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    SpinBoundScope scope(std::chrono::nanoseconds(0));
    constexpr int kThreads = 4;
    constexpr int kRounds = 5000;
    const u32 section = c.Alloc(28);
    c.Kernel("RtlInitializeCriticalSection", {section});
    auto start = std::make_shared<std::latch>(kThreads);
    auto guarded = std::make_shared<u32>(0);
    std::vector<u32> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
      threads.push_back(c.Spawn([section, start, guarded](Caller& t) {
        start->arrive_and_wait();
        for (int n = 0; n < kRounds; ++n) {
          t.Kernel("RtlEnterCriticalSection", {section});
          ++*guarded;
          t.Kernel("RtlLeaveCriticalSection", {section});
        }
      }));
    }
    for (const u32 thread : threads) {
      Join(c, thread);
    }
    CHECK(*guarded == static_cast<u32>(kThreads) * kRounds);
    const Fields after = ReadFields(c, section);
    CHECK(after.lock_count == -1);
    CHECK(after.recursion_count == 0);
    CHECK(after.owning_thread == 0);
  });
}
