/**
 * @file        tests/unit/kernel/lock_order_test.cpp
 * @brief       Tests for the ranked mutexes
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("lock order accepts ascending ranks", "[lock_order]") {
#ifdef NDEBUG
  SKIP("lock order is checked in debug builds only");
#endif
  g_violations = 0;
  auto previous = rex::platform::SetLockOrderHandler(&CountViolation);
  {
    RankedMutex identity(LockRank::Identity);
    RankedSharedMutex table(LockRank::Table);
    RankedMutex name(LockRank::Name);
    RankedMutex object(LockRank::Object);
    std::lock_guard identity_lock(identity);
    std::shared_lock table_lock(table);
    std::lock_guard name_lock(name);
    std::lock_guard object_lock(object);
  }
  rex::platform::SetLockOrderHandler(previous);
  CHECK(g_violations == 0);
}

TEST_CASE("lock order reports a descending acquire", "[lock_order]") {
#ifdef NDEBUG
  SKIP("lock order is checked in debug builds only");
#endif
  g_violations = 0;
  auto previous = rex::platform::SetLockOrderHandler(&CountViolation);
  {
    RankedMutex name(LockRank::Name);
    RankedSharedMutex table(LockRank::Table);
    std::lock_guard name_lock(name);
    std::unique_lock table_lock(table);
  }
  rex::platform::SetLockOrderHandler(previous);
  CHECK(g_violations == 1);
}

TEST_CASE("a leaf lock may follow any lock and nothing may follow a leaf", "[lock_order]") {
#ifdef NDEBUG
  SKIP("lock order is checked in debug builds only");
#endif
  g_violations = 0;
  auto previous = rex::platform::SetLockOrderHandler(&CountViolation);
  {
    RankedMutex registry(LockRank::ThreadRegistry);
    RankedMutex leaf(LockRank::Leaf);
    RankedMutex object(LockRank::Object);
    std::lock_guard registry_lock(registry);
    std::lock_guard leaf_lock(leaf);
    CHECK(g_violations == 0);
    std::lock_guard object_lock(object);
    CHECK(g_violations == 1);
  }
  rex::platform::SetLockOrderHandler(previous);
}

TEST_CASE("lock order is tracked per thread", "[lock_order]") {
#ifdef NDEBUG
  SKIP("lock order is checked in debug builds only");
#endif
  g_violations = 0;
  auto previous = rex::platform::SetLockOrderHandler(&CountViolation);
  RankedMutex object(LockRank::Object);
  RankedMutex name(LockRank::Name);
  std::lock_guard object_lock(object);
  std::thread other([&] { std::lock_guard name_lock(name); });
  other.join();
  rex::platform::SetLockOrderHandler(previous);
  CHECK(g_violations == 0);
}
