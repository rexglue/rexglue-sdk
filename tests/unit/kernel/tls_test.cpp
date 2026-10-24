/**
 * @file        tests/unit/kernel/tls_test.cpp
 * @brief       TLS slots and the TLS block against the XDK rules
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "kernel_fixture.h"
#include "runtime/guest_stack.h"

using rex::u32;
using rex::test::Caller;
using rex::test::Join;
using rex::test::KernelFixture;

namespace {

constexpr u32 kOutOfIndexes = 0xFFFFFFFF;

}  // namespace

REX_TEST_CASE("ResolveTLSSlotCount keeps 32 to 256 slots", "[kernel][tls]") {
  CHECK(rex::runtime::ResolveTLSSlotCount(0) == 32);
  CHECK(rex::runtime::ResolveTLSSlotCount(10) == 32);
  CHECK(rex::runtime::ResolveTLSSlotCount(31) == 32);
  CHECK(rex::runtime::ResolveTLSSlotCount(32) == 32);
  CHECK(rex::runtime::ResolveTLSSlotCount(33) == 33);
  CHECK(rex::runtime::ResolveTLSSlotCount(64) == 64);
  CHECK(rex::runtime::ResolveTLSSlotCount(255) == 255);
  CHECK(rex::runtime::ResolveTLSSlotCount(256) == 256);
  CHECK(rex::runtime::ResolveTLSSlotCount(257) == 256);
  CHECK(rex::runtime::ResolveTLSSlotCount(300) == 256);
}

REX_TEST_CASE("A title without a TLS header has 32 slots", "[kernel][tls]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    std::vector<u32> slots;
    for (u32 slot = c.Kernel("KeTlsAlloc"); slot != kOutOfIndexes; slot = c.Kernel("KeTlsAlloc")) {
      slots.push_back(slot);
      CHECKED_ELSE(slots.size() <= 256) {
        FAIL_CHECK("the check above failed");
        return;
      }
    }
    CHECK(slots.size() == 32);
    for (const u32 slot : slots) {
      c.Kernel("KeTlsFree", {slot});
    }
  });
}

REX_TEST_CASE("A slot at or past the slot count is rejected", "[kernel][tls]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    CHECK(c.Kernel("KeTlsSetValue", {31, 9}) == 1);
    CHECK(c.Kernel("KeTlsGetValue", {31}) == 9);
    CHECK(c.Kernel("KeTlsSetValue", {32, 9}) == 0);
    CHECK(c.Kernel("KeTlsGetValue", {32}) == 0);
  });
}

REX_TEST_CASE("KeTlsFree clears the slot in every guest thread", "[kernel][tls]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 slot = c.Kernel("KeTlsAlloc");
    CHECKED_ELSE(slot != kOutOfIndexes) {
      FAIL_CHECK("the check above failed");
      return;
    }
    c.Kernel("KeTlsSetValue", {slot, 5});
    auto stage = std::make_shared<std::atomic<int>>(0);
    auto seen = std::make_shared<std::atomic<u32>>(kOutOfIndexes);
    const u32 handle = c.Spawn([slot, stage, seen](Caller& t) {
      t.Kernel("KeTlsSetValue", {slot, 7});
      stage->store(1);
      while (stage->load() != 2) {
        std::this_thread::yield();
      }
      seen->store(t.Kernel("KeTlsGetValue", {slot}));
    });
    while (stage->load() != 1) {
      std::this_thread::yield();
    }
    CHECK(c.Kernel("KeTlsFree", {slot}) == 1);
    stage->store(2);
    Join(c, handle);
    CHECK(seen->load() == 0);
    CHECK(c.Kernel("KeTlsGetValue", {slot}) == 0);
  });
}

REX_TEST_CASE("KeTlsFree after a thread with the slot exited writes no released block",
              "[kernel][tls]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 slot = c.Kernel("KeTlsAlloc");
    CHECKED_ELSE(slot != kOutOfIndexes) {
      FAIL_CHECK("the check above failed");
      return;
    }
    for (int i = 0; i < 20; ++i) {
      Join(c, c.Spawn([slot](Caller& t) { t.Kernel("KeTlsSetValue", {slot, 3}); }));
    }
    CHECK(c.Kernel("KeTlsFree", {slot}) == 1);
  });
}

REX_TEST_CASE("The TLS data block is copied with a slot count of 0", "[kernel][tls]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 process = c.GetRuntime().GetTitleProcess();
    const u32 raw = c.Alloc(8);
    c.Store32(raw, 0x11223344);
    c.Store32(raw + 4, 0x55667788);
    c.Store32(process + 0x20, raw);
    c.Store32(process + 0x24, 16);
    c.Store32(process + 0x28, 8);
    auto words = std::make_shared<std::vector<u32>>();
    Join(c, c.Spawn([words](Caller& t) {
      const u32 data = t.Load32(t.GetContext().r13.u32);
      const u32 kthread = t.Load32(t.GetContext().r13.u32 + 0x100);
      for (u32 offset = 0; offset < 16; offset += 4) {
        words->push_back(t.Load32(data + offset));
      }
      words->push_back(t.Load32(kthread + 0x68) - data);
    }));
    c.Store32(process + 0x20, 0);
    c.Store32(process + 0x24, 0);
    c.Store32(process + 0x28, 0);
    CHECK(*words == std::vector<u32>{0x11223344, 0x55667788, 0, 0, 16});
  });
}
