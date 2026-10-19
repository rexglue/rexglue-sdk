/**
 * @file        tests/elapsed.h
 * @brief       Elapsed wall time for test assertions
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <chrono>

#include <rex/types.h>

namespace rex::test {

/**
 * Measures the time since a point on the steady clock.
 *
 * @param start The time point to measure from.
 * @return The elapsed milliseconds.
 */
inline i64 MillisSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                               start)
      .count();
}

}  // namespace rex::test
