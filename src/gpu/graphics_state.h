/**
 * @file        gpu/graphics_state.h
 * @brief       Private state of GraphicsSystem
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <stop_token>
#include <thread>

#include <rex/gpu/graphics_driver.h>
#include <rex/gpu/xenos_graphics_driver.h>
#include "gpu/graphics_system.h"
#include <rex/guest.h>
#include <rex/ppc/mmio.h>
#include <rex/title.h>
#include <rex/utility.h>

namespace rex::gpu {

struct GraphicsSystem::State {
  explicit State(GraphicsSystem& owner_system) : owner(owner_system) {}

  void CountFrame();

  void PresentLoop(std::stop_token stop);

  /** Warns once that a Vd call arrived with no Xenos driver. */
  void WarnNoXenos(const char* what);

  u32 ReadNullRegister(u32 index);

  void WriteNullRegister(u32 index, u32 value);

  GraphicsSystem& owner;
  rex::gpu::IGraphicsDriver* driver = nullptr;
  MoveOnlyFunction<bool(std::chrono::milliseconds)> serve_ui;
  rex::gpu::IXenosGraphicsDriver* xenos = nullptr;
  std::jthread present_thread;
  std::atomic<bool> present_finished{false};

  mutable std::mutex stats_mutex;
  std::deque<std::chrono::steady_clock::time_point> frame_times;
  u64 frame_count = 0;

  std::mutex hook_mutex;
  MoveOnlyFunction<void(IOverlayRenderer&)> overlay_hook;

  std::mutex resize_mutex;
  bool resize_pending = false;
  u32 width = 0;
  u32 height = 0;

  TitleInfo title_info;
  IPhysicalMemory* memory = nullptr;
  std::atomic<u32> null_writeback{0};
  std::array<std::atomic<u32>, rex::ppc::kMMIOBlockSize / 4> null_registers{};

  std::atomic<bool> warned{false};
  std::atomic<u32> warning_count{0};
};

}  // namespace rex::gpu
