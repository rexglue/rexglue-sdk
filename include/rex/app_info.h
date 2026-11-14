/**
 * @file        app_info.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string_view>

#include <rex/driver.h>
#include <rex/module.h>

namespace rex {

struct AppSysRequirements {
  GraphicsFamily graphics_family = GraphicsFamily::kNone; /**< kNone for no graphics driver. */
  bool audio = false;
  bool input = false; /**< At least one input driver is required. */
  bool ui = false;    /**< A UI driver with a window is required. */
};

REX_DECLARE_INTERFACE(IAppInfo, 1)
/** Returns what the app requires of the systems. */
  virtual const AppSysRequirements& GetRequirements() const = 0;

  virtual std::string_view GetAppName() const = 0;
};

}  // namespace rex
