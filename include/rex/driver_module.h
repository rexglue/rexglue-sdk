/**
 * @file        driver_module.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/driver.h>
#include <rex/driver_registration.h>
#include <rex/module.h>
#include <rex/result.h>

namespace rex {

class IDriverContext;

/**
 * A module that provides drivers to the runtime. The app opens only the modules named
 * rexapu-<name>, rexhid-<name>, rexui-<name> and rexgpu-<family>-<name> for the selected drivers.
 */
REX_DECLARE_INTERFACE(IDriverModule, 1)
  virtual std::span<const DriverRegistration> GetDrivers() const = 0;
};

/**
 * Names the modules that serve the drivers of a type, in list order.
 *
 * @param names Driver names separated by commas. Empty names, "none" and repeats are skipped.
 * @param family The family of a graphics driver, kNone for the other types.
 * @param connected The connected modules, whose registered names are skipped. Null skips none.
 * @return The module names in list order.
 */
[[nodiscard]] REX_API std::vector<std::string> GetDriverModules(
    DriverType type, std::string_view names, GraphicsFamily family = GraphicsFamily::kNone,
    IFactory* connected = nullptr);

/**
 * Creates and initializes the drivers of a type, in list order.
 *
 * @param factory The factory that finds the connected modules.
 * @param names Driver names separated by commas. Empty, or only "none", selects no driver.
 * @param context The context each driver initializes with.
 * @return The drivers in list order, or an error naming the driver that is missing or failed,
 * after the earlier ones shut down.
 */
[[nodiscard]] REX_API Result<std::vector<std::unique_ptr<IDriver>>> SelectDrivers(
    IFactory& factory, DriverType type, std::string_view names, IDriverContext& context);

}  // namespace rex
