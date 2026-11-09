/**
 * @file        gpu/present_thread.cpp
 * @brief       The present thread of GraphicsSystem
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <thread>

#include "gpu/graphics_state.h"

namespace rex::gpu {

namespace {

constexpr u32 kFrameWaitMs = 100;
constexpr auto kSkipDelay = std::chrono::milliseconds(5);
constexpr auto kSleepStep = std::chrono::milliseconds(20);

void SleepUntil(std::chrono::steady_clock::time_point deadline, const std::stop_token& stop) {
  while (!stop.stop_requested()) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return;
    }
    std::this_thread::sleep_for(
        std::min<std::chrono::steady_clock::duration>(deadline - now, kSleepStep));
  }
}

}  // namespace

void GraphicsSystem::State::PresentLoop(const std::stop_token& stop) {
  auto next_present = std::chrono::steady_clock::now();
  while (!stop.stop_requested()) {
    const rex::gpu::FrameStatus status = driver->WaitForFrame(kFrameWaitMs);
    if (status.new_frame) {
      CountFrame();
    }

    bool resize = false;
    u32 new_width = 0;
    u32 new_height = 0;
    {
      std::lock_guard lock(resize_mutex);
      resize = resize_pending;
      new_width = width;
      new_height = height;
      resize_pending = false;
    }
    if (resize) {
      driver->ResizeSurface(new_width, new_height);
    }

    if (!driver->BeginPresent()) {
      SleepUntil(std::chrono::steady_clock::now() + kSkipDelay, stop);
      continue;
    }
    {
      std::lock_guard lock(hook_mutex);
      if (overlay_hook) {
        overlay_hook(*driver);
      }
    }
    driver->EndPresent();

    const f64 limit = owner.GetSettings().frame_limit.Get();
    const auto now = std::chrono::steady_clock::now();
    if (limit > 0.0) {
      const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<f64>(1.0 / limit));
      next_present += period;
      if (next_present + period < now) {
        next_present = now;
      }
      SleepUntil(next_present, stop);
    } else {
      next_present = now;
    }
  }
}

}  // namespace rex::gpu
