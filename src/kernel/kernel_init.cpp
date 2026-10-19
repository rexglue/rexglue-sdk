#include <rex/kernel/init.h>
#include <rex/kernel/xam/apps/app.h>
#include <rex/kernel/xam/apps/xgi_app.h>
#include <rex/kernel/xam/apps/xlivebase_app.h>
#include <rex/kernel/xam/apps/xmp_app.h>
#include <rex/kernel/xam/module.h>
#include <rex/kernel/xbdm/module.h>
#include <rex/kernel/xboxkrnl/module.h>
#include <rex/runtime.h>
#include <rex/runtime/content_manager.h>
#include <rex/runtime/module_registry.h>

namespace rex::kernel {

Result<void> InitializeKernel(Runtime& runtime) {
  auto& apps = runtime.GetContentManager().GetAppManager();
  apps.RegisterApp(std::make_unique<xam::apps::XMPApp>(runtime));
  apps.RegisterApp(std::make_unique<xam::apps::XGIApp>(runtime));
  apps.RegisterApp(std::make_unique<xam::apps::XLiveBaseApp>(runtime));
  apps.RegisterApp(std::make_unique<xam::apps::XAMApp>(runtime));

  auto& modules = runtime.GetModuleRegistry();
  REX_TRY_VOID(modules.RegisterKernelModule(xboxkrnl::CreateXboxkrnlModule(runtime)));
  REX_TRY_VOID(modules.RegisterKernelModule(xam::CreateXAMModule(runtime)));
  REX_TRY_VOID(modules.RegisterKernelModule(xbdm::CreateXBDMModule(runtime)));
  return Ok();
}

}  // namespace rex::kernel
