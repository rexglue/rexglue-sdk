/**
 * @file        tests/unit/kernel/lock_order_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <thread>

#include <rex/platform/lock_order.h>

using rex::platform::LockRank;
using rex::platform::RankedMutex;
using rex::platform::RankedSharedMutex;

namespace {
std::atomic<int> g_violations{0};
void CountViolation(LockRank, LockRank) {
  ++g_violations;
}
}  // namespace

REX_TEST_CASE("lock order accepts ascending ranks", "[lock_order]") {
#ifdef NDEBUG
  SKIP("lock order is checked in debug builds only");
#endif
  g_violations = 0;
  auto previous = rex::platform::SetLockOrderHandler(&CountViolation);
  {
    RankedMutex identity(LockRank::kIdentity);
    RankedSharedMutex table(LockRank::kTable);
    RankedMutex name(LockRank::kName);
    RankedMutex object(LockRank::kObject);
    std::lock_guard identity_lock(identity);
    std::shared_lock table_lock(table);
    std::lock_guard name_lock(name);
    std::lock_guard object_lock(object);
  }
  rex::platform::SetLockOrderHandler(previous);
  CHECK(g_violations == 0);
}

REX_TEST_CASE("lock order reports a descending acquire", "[lock_order]") {
#ifdef NDEBUG
  SKIP("lock order is checked in debug builds only");
#endif
  g_violations = 0;
  auto previous = rex::platform::SetLockOrderHandler(&CountViolation);
  {
    RankedMutex name(LockRank::kName);
    RankedSharedMutex table(LockRank::kTable);
    std::lock_guard name_lock(name);
    std::unique_lock table_lock(table);
  }
  rex::platform::SetLockOrderHandler(previous);
  CHECK(g_violations == 1);
}

REX_TEST_CASE("a leaf lock may follow any lock and nothing may follow a leaf", "[lock_order]") {
#ifdef NDEBUG
  SKIP("lock order is checked in debug builds only");
#endif
  g_violations = 0;
  auto previous = rex::platform::SetLockOrderHandler(&CountViolation);
  {
    RankedMutex registry(LockRank::kThreadRegistry);
    RankedMutex leaf(LockRank::kLeaf);
    RankedMutex object(LockRank::kObject);
    std::lock_guard registry_lock(registry);
    std::lock_guard leaf_lock(leaf);
    CHECK(g_violations == 0);
    std::lock_guard object_lock(object);
    CHECK(g_violations == 1);
  }
  rex::platform::SetLockOrderHandler(previous);
}

REX_TEST_CASE("an interrupt lock comes before every other rank", "[lock_order]") {
#ifdef NDEBUG
  SKIP("lock order is checked in debug builds only");
#endif
  g_violations = 0;
  auto previous = rex::platform::SetLockOrderHandler(&CountViolation);
  {
    RankedMutex interrupt(LockRank::kInterrupt);
    RankedMutex identity(LockRank::kIdentity);
    RankedMutex leaf(LockRank::kLeaf);
    {
      std::lock_guard interrupt_lock(interrupt);
      std::lock_guard identity_lock(identity);
      std::lock_guard leaf_lock(leaf);
    }
    CHECK(g_violations == 0);
    std::lock_guard identity_lock(identity);
    rex::platform::PushLockRank(LockRank::kInterrupt);
    rex::platform::PopLockRank(LockRank::kInterrupt);
    CHECK(g_violations == 1);
  }
  rex::platform::SetLockOrderHandler(previous);
}

REX_TEST_CASE("lock order is tracked per thread", "[lock_order]") {
#ifdef NDEBUG
  SKIP("lock order is checked in debug builds only");
#endif
  g_violations = 0;
  auto previous = rex::platform::SetLockOrderHandler(&CountViolation);
  RankedMutex object(LockRank::kObject);
  RankedMutex name(LockRank::kName);
  std::lock_guard object_lock(object);
  std::thread other([&] { std::lock_guard name_lock(name); });
  other.join();
  rex::platform::SetLockOrderHandler(previous);
  CHECK(g_violations == 0);
}
