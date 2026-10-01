/**
 * @file        kernel/kernel_init.cpp
 * @brief       Kernel initialization - loads kernel modules and registers apps
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/kernel/init.h>
#include <rex/kernel/xam/apps/app.h>
#include <rex/kernel/xam/apps/xgi_app.h>
#include <rex/kernel/xam/apps/xlivebase_app.h>
#include <rex/kernel/xam/apps/xmp_app.h>
#include <rex/kernel/xam/module.h>
#include <rex/kernel/xbdm/module.h>
#include <rex/kernel/xboxkrnl/module.h>
#include <rex/runtime.h>

namespace rex::kernel {

void InitializeKernel(Runtime& runtime) {
  auto& apps = runtime.content_manager().app_manager();
  apps.RegisterApp(std::make_unique<xam::apps::XmpApp>(runtime));
  apps.RegisterApp(std::make_unique<xam::apps::XgiApp>(runtime));
  apps.RegisterApp(std::make_unique<xam::apps::XLiveBaseApp>(runtime));
  apps.RegisterApp(std::make_unique<xam::apps::XamApp>(runtime));

  auto& modules = runtime.module_registry();
  (void)modules.LoadKernelModule(xboxkrnl::CreateXboxkrnlModule(runtime));
  (void)modules.LoadKernelModule(xam::CreateXamModule(runtime));
  (void)modules.LoadKernelModule(xbdm::CreateXbdmModule(runtime));
}

}  // namespace rex::kernel
