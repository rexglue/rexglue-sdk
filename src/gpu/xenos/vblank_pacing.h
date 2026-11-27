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

#include <algorithm>

#include <rex/types.h>

namespace rex::gpu {

/** Most vertical blanks one pass delivers before the missed ones are skipped. */
inline constexpr u32 kMaxVblankCatchUp = 4;

/**
 * Returns the guest ticks between vertical blanks.
 *
 * @param vblank_rate_hz The configured rate. A value that is not above 0 selects the refresh rate.
 * @param refresh_rate_hz The refresh rate of the title's video mode.
 * @param tick_frequency Guest ticks per second.
 */
inline u64 GetVblankIntervalTicks(f64 vblank_rate_hz, f64 refresh_rate_hz, u64 tick_frequency) {
  const f64 rate_hz = vblank_rate_hz > 0.0 ? std::clamp(vblank_rate_hz, 1.0, 1000.0)
                                           : std::max(1.0, refresh_rate_hz);
  return std::max<u64>(1, static_cast<u64>(static_cast<f64>(tick_frequency) / rate_hz));
}

/**
 * Starts the blank schedule over when the interval changes, so the blanks owed at the old interval
 * are not delivered at the new one.
 *
 * @param now Guest ticks at the start of the pass.
 * @param last_frame_time Guest ticks of the last scheduled blank.
 * @param scheduled_interval The interval the schedule has run at, 0 before the first pass.
 * @param interval_ticks The interval for this pass, at least 1.
 */
inline void ApplyVblankInterval(u64 now, u64& last_frame_time, u64& scheduled_interval,
                                u64 interval_ticks) {
  if (interval_ticks == scheduled_interval) {
    return;
  }
  if (scheduled_interval != 0 && now - last_frame_time > interval_ticks) {
    last_frame_time = now - interval_ticks;
  }
  scheduled_interval = interval_ticks;
}

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
