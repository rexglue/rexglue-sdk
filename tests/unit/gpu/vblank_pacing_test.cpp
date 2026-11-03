/**
 * @file        tests/unit/gpu/vblank_pacing_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include "gpu/xenos/vblank_pacing.h"

using namespace rex::gpu;

namespace {

constexpr rex::u64 kInterval = 100;

}  // namespace

REX_TEST_CASE("Nothing is due before one interval has passed", "[gpu][vblank]") {
  rex::u64 last = 1000;
  rex::u32 marks = 0;
  const rex::u32 delivered = DeliverVblanks(
      1099, last, kInterval, kMaxVblankCatchUp, [&] { ++marks; }, [] { return true; });
  CHECK(delivered == 0);
  CHECK(marks == 0);
  CHECK(last == 1000);
}

REX_TEST_CASE("Each interval that has passed delivers one blank", "[gpu][vblank]") {
  rex::u64 last = 1000;
  rex::u32 marks = 0;
  const rex::u32 delivered = DeliverVblanks(
      1250, last, kInterval, kMaxVblankCatchUp, [&] { ++marks; }, [] { return true; });
  CHECK(delivered == 2);
  CHECK(marks == 2);
  CHECK(last == 1200);
}

REX_TEST_CASE("A pass far behind delivers the limit and restarts the schedule", "[gpu][vblank]") {
  rex::u64 last = 0;
  rex::u32 marks = 0;
  const rex::u32 delivered = DeliverVblanks(
      1'000'000, last, kInterval, kMaxVblankCatchUp, [&] { ++marks; }, [] { return true; });
  CHECK(delivered == kMaxVblankCatchUp);
  CHECK(marks == kMaxVblankCatchUp);
  CHECK(last == 1'000'000);
}

REX_TEST_CASE("A slow blank handler does not extend the pass", "[gpu][vblank]") {
  rex::u64 last = 0;
  rex::u64 clock = 1'000;
  rex::u32 marks = 0;
  DeliverVblanks(
      clock, last, kInterval, kMaxVblankCatchUp,
      [&] {
        ++marks;
        clock += 10 * kInterval;
      },
      [] { return true; });
  CHECK(marks == kMaxVblankCatchUp);
}

REX_TEST_CASE("A pass stops as soon as the worker is told to stop", "[gpu][vblank]") {
  rex::u64 last = 0;
  rex::u32 marks = 0;
  bool running = true;
  DeliverVblanks(
      1'000, last, kInterval, 100,
      [&] {
        if (++marks == 2) {
          running = false;
        }
      },
      [&] { return running; });
  CHECK(marks == 2);
}
