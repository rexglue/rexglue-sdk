/**
 * @file        tests/unit/base/module_registration.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <vector>

#include <rex/driver.h>
#include <rex/driver_registration.h>
#include <rex/driver_module.h>

#include "base/module_group.h"

namespace rex::test {

inline std::vector<const DriverRegistration*> GetRegistrations(ModuleGroup& group,
                                                               DriverType type) {
  std::vector<const DriverRegistration*> registrations;
  for (IDriverModule* module : group.FindAll<IDriverModule>()) {
    for (const DriverRegistration& registration : module->GetDrivers()) {
      if (registration.type == type) {
        registrations.push_back(&registration);
      }
    }
  }
  return registrations;
}

}  // namespace rex::test
