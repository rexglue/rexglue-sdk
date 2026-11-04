/**
 * @file        gpu/graphics_system.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>

#include <rex/module.h>
#include <rex/platform.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>
#include <rex/types.h>

namespace rex::gpu {

struct FrameStats {
  f64 frame_time_ms = 0.0; /**< Average time between title frames over the last second. */
  f64 fps = 0.0;
  u64 frame_count = 0; /**< Title frames since launch. */
};

class REX_GPU_API GraphicsSettings final : public SettingOwner {
 public:
  GraphicsSettings();

  REX_SETTING(
      std::string, driver, "any",
      "Graphics driver within the title's family. Values are d3d12, vulkan, null, none for no "
      "driver in an app that does not require one, or any for d3d12 on Windows and vulkan "
      "elsewhere",
      kStartupOnly);
  REX_SETTING(bool, vsync, true, "Wait for vertical blank when presenting");
  REX_SETTING(f64, frame_limit, 0.0, "Most presents per second, 0 for no limit",
              Range{0.0, 1000.0});
  REX_SETTING(i32, resolution_scale_x, 1, "Draw resolution scale X", Range{1, 8}, kRestart);
  REX_SETTING(i32, resolution_scale_y, 1, "Draw resolution scale Y", Range{1, 8}, kRestart);
  REX_SETTING(i32, anisotropic_override, 3,
              "Anisotropic filtering, -1 for the title's choice, 0 for off, 1 to 5 for 1x to 16x",
              Range{-1, 5});
};

/**
 * Runs the present thread when a driver exists and routes the guest kernel's
 * Vd calls and GPU register accesses to a Xenos driver. With no driver a title that presents
 * itself calls ReportFrame and UISystem::DrawOverlays from its own present.
 */
REX_DECLARE_INTERFACE(IGraphicsSystem, 1)
  virtual GraphicsSettings& GetSettings() = 0;

  virtual bool HasDriver() const = 0;

  /** Returns the name of the selected driver, with any resolved to d3d12 or vulkan, or none. */
  virtual std::string GetDriverName() const = 0;

  /**
   * Counts one title frame, for a title that presents with its own renderer. Any thread.
   */
  virtual void ReportFrame() = 0;

  /** Returns the frame counters. Any thread. The result is a snapshot. */
  virtual FrameStats GetFrameStats() const = 0;
};

}  // namespace rex::gpu
