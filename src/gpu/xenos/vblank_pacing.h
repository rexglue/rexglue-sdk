/**
 * @file        gpu/xenos/vblank_pacing.h
 * @brief       Catch up on the vertical blanks that are due without running behind forever
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/types.h>

namespace rex::gpu {

/** Most vertical blanks one pass delivers before the missed ones are skipped. */
inline constexpr u32 kMaxVblankCatchUp = 4;

/**
 * Delivers the vertical blanks that are due at one instant.
 *
 * The instant is fixed for the pass, so a slow handler cannot keep the pass going. When more
 * than the limit are due, the rest are skipped and the schedule restarts at the instant.
 *
 * @tparam Mark Callable taking no arguments that delivers one vertical blank.
 * @tparam Running Callable returning false once delivery should stop.
 * @param now Guest ticks at the start of the pass.
 * @param last_frame_time Guest ticks of the last scheduled blank, advanced by each delivered one.
 * @param interval_ticks Guest ticks between blanks, at least 1.
 * @param max_catch_up Most blanks to deliver in this pass.
 * @param mark Delivers one blank.
 * @param running Reports whether delivery may continue.
 * @return The number of blanks delivered.
 */
template <typename Mark, typename Running>
u32 DeliverVblanks(u64 now, u64& last_frame_time, u64 interval_ticks, u32 max_catch_up, Mark&& mark,
                   Running&& running) {
  u32 delivered = 0;
  while (running() && now - last_frame_time >= interval_ticks) {
    if (delivered == max_catch_up) {
      last_frame_time = now;
      break;
    }
    mark();
    last_frame_time += interval_ticks;
    ++delivered;
  }
  return delivered;
}

}  // namespace rex::gpu
