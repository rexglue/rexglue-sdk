/**
 * @file        tests/core/propagate_const_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <memory>
#include <type_traits>
#include <utility>

#include <rex/propagate_const.h>
#include <rex/types.h>

namespace {

struct Counter {
  void Increment() { ++value; }
  u32 Read() const { return value; }
  u32 value = 0;
};

template <typename P>
constexpr bool kCanMutate = requires(P& pointer) { pointer->Increment(); };

template <typename P>
constexpr bool kCanRead = requires(P& pointer) { pointer->Read(); };

using OwnedCounter = rex::PropagateConst<std::unique_ptr<Counter>>;
using BorrowedCounter = rex::PropagateConst<Counter*>;

}  // namespace

REX_TEST_CASE("PropagateConst refuses a mutating call through a const owner",
              "[core][propagate_const]") {
  CHECK(kCanMutate<OwnedCounter>);
  CHECK_FALSE(kCanMutate<const OwnedCounter>);
  CHECK(kCanRead<const OwnedCounter>);
  CHECK(kCanMutate<BorrowedCounter>);
  CHECK_FALSE(kCanMutate<const BorrowedCounter>);
  CHECK_FALSE(std::is_copy_constructible_v<BorrowedCounter>);
}

REX_TEST_CASE("PropagateConst reaches the pointee through a non-const owner",
              "[core][propagate_const]") {
  OwnedCounter owned(std::make_unique<Counter>());
  owned->Increment();
  (*owned).Increment();
  const OwnedCounter& view = owned;
  CHECK(view->Read() == 2);
  CHECK(view.get() == owned.get());
}

REX_TEST_CASE("PropagateConst moves ownership and reports an empty pointer",
              "[core][propagate_const]") {
  OwnedCounter source(std::make_unique<Counter>());
  source->Increment();
  OwnedCounter target(std::move(source));
  CHECK(target);
  CHECK(target->Read() == 1);
  OwnedCounter empty;
  CHECK_FALSE(empty);
  CHECK(empty == nullptr);
  CHECK(target != nullptr);
  empty = std::make_unique<Counter>();
  CHECK(empty);
  empty = nullptr;
  CHECK_FALSE(empty);
}
