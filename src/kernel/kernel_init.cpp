/**
 * @file        kernel/kernel_init.cpp
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

Result<void> InitializeKernel(Runtime& runtime) {
  auto& apps = runtime.GetContentManager().GetAppManager();
  apps.RegisterApp(std::make_unique<xam::apps::XmpApp>(runtime));
  apps.RegisterApp(std::make_unique<xam::apps::XgiApp>(runtime));
  apps.RegisterApp(std::make_unique<xam::apps::XLiveBaseApp>(runtime));
  apps.RegisterApp(std::make_unique<xam::apps::XAMApp>(runtime));

  auto& modules = runtime.GetModuleRegistry();
  REX_TRY_VOID(modules.LoadKernelModule(xboxkrnl::CreateXboxkrnlModule(runtime)));
  REX_TRY_VOID(modules.LoadKernelModule(xam::CreateXAMModule(runtime)));
  REX_TRY_VOID(modules.LoadKernelModule(xbdm::CreateXBDMModule(runtime)));
  return Ok();
}

}  // namespace rex::kernel
