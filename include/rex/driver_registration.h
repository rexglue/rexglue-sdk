/**
 * @file        driver_registration.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <string_view>

#include <rex/driver.h>

namespace rex {

struct DriverRegistration {
  DriverType type;
  std::string_view name; /**< The name players select it by. */
  GraphicsFamily family; /**< The graphics family, kNone for the other driver types. */
  /** Creates the driver. Called only when it is selected. */
  std::unique_ptr<IDriver> (*create)();
};

}  // namespace rex
