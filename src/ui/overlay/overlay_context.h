/**
 * @file        ui/overlay/overlay_context.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/gpu/graphics_system.h>
#include <rex/utility.h>
#include <rex/ui/ui_system.h>

namespace rex {
class IAchievementRegistry;
namespace hid {
class InputSettings;
}  // namespace hid
class ISettingsRegistry;
namespace hid {
class IBindingSet;
class IInputSystem;
}  // namespace hid
namespace log {
class LogCaptureSink;
}  // namespace log
}  // namespace rex

namespace rex::ui {

class IConsoleHost;
class ConsoleModel;

using FrameStatsProvider = MoveOnlyFunction<rex::gpu::FrameStats()>;

struct OverlayContext {
  IUISystem& ui;
  ISettingsRegistry& settings;
  /** The keyboard bindings the overlays check for conflicts, null without an input system. */
  const hid::IBindingSet* key_bindings = nullptr;
  /** The input system that reads and writes key chords, null without one. */
  const hid::IInputSystem* input = nullptr;
  /** The input settings the overlays read, null without an input system. */
  const rex::hid::InputSettings* input_settings = nullptr;
};

struct ModuleOverlayContext {
  OverlayContext base;
  ConsoleModel& console;
  IConsoleHost& console_host;
  log::LogCaptureSink& log_capture;
  IAchievementRegistry*& achievements;
  FrameStatsProvider& frame_stats;
};

}  // namespace rex::ui
