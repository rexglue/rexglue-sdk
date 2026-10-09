/**
 * @file        ui/sdl3/overlay_context.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/ui/overlay.h>
#include <rex/ui/user_interface.h>

namespace rex {
class Runtime;
namespace log {
class CaptureSink;
}  // namespace log
}  // namespace rex

namespace rex::ui {

class ConsoleHost;
class ConsoleModel;

struct ModuleOverlayContext {
  OverlayContext base;
  ConsoleModel& console;
  ConsoleHost& console_host;
  log::CaptureSink& log_capture;
  Runtime*& runtime;
  FrameStatsProvider& frame_stats;
};

}  // namespace rex::ui
