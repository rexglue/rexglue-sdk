/**
 * @file        kernel/kernel_init.cpp
 * @brief       Kernel initialization - loads HLE modules and registers apps
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/kernel/kernel_init.h>

#include <rex/kernel/xam/apps/app.h>
#include <rex/kernel/xam/apps/xgi_app.h>
#include <rex/kernel/xam/apps/xlivebase_app.h>
#include <rex/kernel/xam/apps/xmp_app.h>
#include <rex/kernel/xam/module.h>
#include <rex/kernel/xboxkrnl/module.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/app_manager.h>

namespace rex::kernel {

void InitKernel(Runtime* runtime) {
  auto* kernel_state = runtime->kernel_state();

  // Load HLE kernel modules
  kernel_state->LoadKernelModule<xboxkrnl::XboxkrnlModule>();
  kernel_state->LoadKernelModule<xam::XamModule>();

  // Register kernel apps
  auto* manager = kernel_state->app_manager();
  manager->RegisterApp(std::make_unique<xam::apps::XmpApp>(kernel_state));
  manager->RegisterApp(std::make_unique<xam::apps::XgiApp>(kernel_state));
  manager->RegisterApp(std::make_unique<xam::apps::XLiveBaseApp>(kernel_state));
  manager->RegisterApp(std::make_unique<xam::apps::XamApp>(kernel_state));
}

}  // namespace rex::kernel
