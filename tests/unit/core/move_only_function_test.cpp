/**
 * @file        tests/unit/core/move_only_function_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <utility>

#include <rex/move_only_function.h>
#include <rex/types.h>

namespace {

template <typename Signature>
using Holder = typename rex::detail::MoveOnlyFunctionFor<Signature>::type;

i32 Twice(i32 value) {
  return value * 2;
}

struct Counted {
  explicit Counted(i32* destroyed) : destroyed_(destroyed) {}
  Counted(Counted&& other) noexcept : destroyed_(std::exchange(other.destroyed_, nullptr)) {}
  Counted(const Counted&) = delete;
  Counted& operator=(const Counted&) = delete;
  Counted& operator=(Counted&&) = delete;
  ~Counted() {
    if (destroyed_) {
      ++*destroyed_;
    }
  }
  i32 operator()() const { return 7; }

  i32* destroyed_;
};

struct LargeCounted : Counted {
  using Counted::Counted;
  std::array<u64, 8> padding{};
};

}  // namespace

TEST_CASE("move only function holder starts empty from default, nullptr and a null pointer",
          "[core][move_only_function]") {
  CHECK_FALSE(Holder<void()>{});
  const bool empty_from_nullptr = Holder<void()>{nullptr} == nullptr;
  CHECK(empty_from_nullptr);
  i32 (*null_function)(i32) = nullptr;
  CHECK_FALSE(Holder<i32(i32)>{null_function});
}

TEST_CASE("move only function holder calls inline and heap targets", "[core][move_only_function]") {
  Holder<i32(i32)> pointer{&Twice};
  REQUIRE(pointer);
  CHECK(pointer(4) == 8);

  std::array<u64, 8> big{};
  big[7] = 5;
  Holder<i32(i32)> heap{[big](i32 value) { return static_cast<i32>(big[7]) + value; }};
  CHECK(heap(1) == 6);
}

TEST_CASE("move only function holder owns a move only capture", "[core][move_only_function]") {
  auto owned = std::make_unique<i32>(9);
  Holder<i32()> f{[p = std::move(owned)] { return *p; }};
  CHECK(f() == 9);
}

TEST_CASE("move only function holder with a const signature calls a const target",
          "[core][move_only_function]") {
  struct ConstOnly {
    i32 operator()() const { return 3; }
  };
  const Holder<i32() const> f{ConstOnly{}};
  CHECK(f() == 3);
}

TEST_CASE("move only function holder move leaves the source empty", "[core][move_only_function]") {
  Holder<i32(i32)> source{&Twice};
  Holder<i32(i32)> moved{std::move(source)};
  CHECK_FALSE(source);
  CHECK(moved(2) == 4);

  Holder<i32(i32)> assigned;
  assigned = std::move(moved);
  CHECK_FALSE(moved);
  CHECK(assigned(3) == 6);
}

TEST_CASE("move only function holder swap exchanges targets", "[core][move_only_function]") {
  Holder<i32()> a{[] { return 1; }};
  Holder<i32()> b{[] { return 2; }};
  swap(a, b);
  CHECK(a() == 2);
  CHECK(b() == 1);
  Holder<i32()> empty;
  a.swap(empty);
  CHECK_FALSE(a);
  CHECK(empty() == 2);
}

TEST_CASE("move only function holder destroys each target exactly once",
          "[core][move_only_function]") {
  i32 inline_destroyed = 0;
  i32 heap_destroyed = 0;
  {
    Holder<i32()> inline_target{Counted{&inline_destroyed}};
    Holder<i32()> heap_target{LargeCounted{&heap_destroyed}};
    Holder<i32()> inline_moved{std::move(inline_target)};
    Holder<i32()> heap_moved;
    heap_moved = std::move(heap_target);
    CHECK(inline_moved() == 7);
    CHECK(heap_moved() == 7);
    CHECK(inline_destroyed == 0);
    CHECK(heap_destroyed == 0);
  }
  CHECK(inline_destroyed == 1);
  CHECK(heap_destroyed == 1);

  i32 reset_destroyed = 0;
  Holder<i32()> reset{Counted{&reset_destroyed}};
  reset = nullptr;
  CHECK(reset_destroyed == 1);
  CHECK_FALSE(reset);
}
