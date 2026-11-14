#include "kernel/kernel_init.h"
#include <rex/kernel/title_runtime.h>
#include <rex/xam/content_manager.h>
#include <rex/kernel/module.h>

namespace rex::kernel {

Result<void> InitializeKernel(TitleRuntime& runtime) {
  auto& modules = runtime.GetModuleRegistry();
  REX_TRY_VOID(modules.RegisterKernelModule(xboxkrnl::CreateXboxkrnlModule(runtime)));
  REX_TRY_VOID(
      modules.RegisterKernelModule(ModuleObject::CreateKernelModule(runtime, "xe:\\xam.xex")));
  REX_TRY_VOID(modules.RegisterKernelModule(xbdm::CreateXBDMModule(runtime)));
  return Ok();
}

}  // namespace rex::kernel
