/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstring>
#include <memory>
#include <vector>

#include <format>

#include <rex/kernel/title_runtime.h>
#include <rex/kernel/kernel_settings.h>
#include <rex/chrono.h>
#include "kernel/kernel_init.h"
#include <rex/kernel/xboxkrnl.h>
#include <rex/logging/log_category.h>
#include <rex/math.h>
#include <rex/ppc/context.h>
#include <rex/ppc/hook.h>
#include <rex/types.h>
#include <rex/kernel/object/module_object.h>
#include <rex/kernel/object/kernel_thread.h>
#include <rex/platform/timer.h>
#include "kernel/xboxkrnl/loaded_command_line.h"
#include "kernel/xboxkrnl/video_export.h"
#include "kernel/module_registry.h"
#include "kernel/settings_access.h"
#include "kernel/title_state.h"

namespace rex::kernel::xboxkrnl {
using namespace rex::kernel;

namespace {

struct X_HARDWARE_INFO {
  be_u32 flags;
  u8 cpu_count;
  u8 reserved[11];
};
REX_STATIC_ASSERT_SIZE(X_HARDWARE_INFO, 0x10);
static_assert(offsetof(X_HARDWARE_INFO, cpu_count) == 4);

struct X_KERNEL_VERSION {
  be_u16 major;
  be_u16 minor;
  be_u16 build;
  be_u16 qfe;
};
REX_STATIC_ASSERT_SIZE(X_KERNEL_VERSION, 8);

struct X_KE_TIMESTAMP_BUNDLE {
  be_u64 interrupt_time;
  be_u64 system_time;
  be_u32 tick_count;
  be_u32 reserved;
};
REX_STATIC_ASSERT_SIZE(X_KE_TIMESTAMP_BUNDLE, 0x18);
static_assert(offsetof(X_KE_TIMESTAMP_BUNDLE, tick_count) == 0x10);

}  // namespace

X_HANDLE CreateXboxkrnlModule(TitleRuntime& runtime) {
  const X_HANDLE handle = ModuleObject::CreateKernelModule(runtime, "xe:\\xboxkrnl.exe");
  if (!handle) {
    return 0;
  }
  auto module_pin = runtime.GetObjectTable().Get<ModuleObject>(handle);
  ModuleObject& module = *module_pin;
  auto* mem = StateOf(runtime).GetAddressSpace();

  RegisterVideoExports(module, runtime);

  for (const u32 ordinal : {0x0059u, 0x0266u}) {
    const u32 monitor_data = mem->SystemHeapAlloc(4);
    *mem->TranslateVirtual<be_u32*>(monitor_data) = 0;
    module.SetVariable(ordinal, monitor_data);
  }

  // Titles test bit 5 (0x20) of flags. Bit 27 (debug mode) breaks allocations when set.
  u32 p_xbox_hardware_info = mem->SystemHeapAlloc(sizeof(X_HARDWARE_INFO));
  auto* hardware_info = mem->TranslateVirtual<X_HARDWARE_INFO*>(p_xbox_hardware_info);
  module.SetVariable(0x0156, p_xbox_hardware_info);
  hardware_info->flags = 0x20;
  hardware_info->cpu_count = 0x06;

  // All 0xFF satisfies any region check.
  u32 p_ex_console_game_region = mem->SystemHeapAlloc(4);
  auto lp_ex_console_game_region = mem->TranslateVirtual(p_ex_console_game_region);
  module.SetVariable(0x000C, p_ex_console_game_region);
  memory::store<u32>(lp_ex_console_game_region, 0xFFFFFFFF);

  // Titles dereference this to find the XexHeaderBase used by RtlImageXexHeaderField.
  u32 pp_xex_executable_module_handle = mem->SystemHeapAlloc(4);
  module.SetVariable(0x0193, pp_xex_executable_module_handle);

  // The full path of the loaded image including its name.
  u32 pp_ex_loaded_image_name = mem->SystemHeapAlloc(kExLoadedImageNameSize);
  module.SetVariable(0x01AF, pp_ex_loaded_image_name);

  const std::string arguments = StateOf(runtime).GetKernelSettings().command_line.Get();
  const std::string command_line = kernel::LoadedCommandLine("default.xex", arguments);
  const u32 command_line_length = kernel::LoadedCommandLineCapacity(arguments);
  u32 p_ex_loaded_command_line = mem->SystemHeapAlloc(command_line_length);
  auto lp_ex_loaded_command_line = mem->TranslateVirtual(p_ex_loaded_command_line);
  module.SetVariable(0x01AE, p_ex_loaded_command_line);
  std::memset(lp_ex_loaded_command_line, 0, command_line_length);
  std::memcpy(lp_ex_loaded_command_line, command_line.c_str(), command_line.length());

  // Titles only compare the version with >=.
  u32 p_xbox_krnl_version = mem->SystemHeapAlloc(sizeof(X_KERNEL_VERSION));
  auto* kernel_version = mem->TranslateVirtual<X_KERNEL_VERSION*>(p_xbox_krnl_version);
  module.SetVariable(0x0158, p_xbox_krnl_version);
  kernel_version->major = 2;
  kernel_version->minor = 0xFFFF;
  kernel_version->build = 0xFFFF;
  kernel_version->qfe = 0x8000;

  // Updated at 1 ms intervals by a system timer.
  u32 p_ke_time_stamp_bundle = mem->SystemHeapAlloc(sizeof(X_KE_TIMESTAMP_BUNDLE));
  auto* timestamp_bundle = mem->TranslateVirtual<X_KE_TIMESTAMP_BUNDLE*>(p_ke_time_stamp_bundle);
  module.SetVariable(0x00AD, p_ke_time_stamp_bundle);
  timestamp_bundle->interrupt_time = 0;
  timestamp_bundle->system_time = 0;
  timestamp_bundle->tick_count = Clock::GetGuestUptimeMillis();
  timestamp_bundle->reserved = 0;
  module.kernel.timestamp_timer = rex::platform::HighResolutionTimer::CreateRepeating(
      std::chrono::milliseconds(1),
      [timestamp_bundle]() { timestamp_bundle->tick_count = Clock::GetGuestUptimeMillis(); });

  auto kernel_globals = StateOf(runtime).GetKernelGlobals();
  REX_ASSERT(kernel_globals != 0);
  module.SetVariable(0x001B, kernel_globals + offsetof(KernelGuestGlobals, ExThreadObjectType));
  module.SetVariable(0x000E, kernel_globals + offsetof(KernelGuestGlobals, ExEventObjectType));
  module.SetVariable(0x0012, kernel_globals + offsetof(KernelGuestGlobals, ExMutantObjectType));
  module.SetVariable(0x0017, kernel_globals + offsetof(KernelGuestGlobals, ExSemaphoreObjectType));
  module.SetVariable(0x001C, kernel_globals + offsetof(KernelGuestGlobals, ExTimerObjectType));
  module.SetVariable(0x0036, kernel_globals + offsetof(KernelGuestGlobals, IoCompletionObjectType));
  module.SetVariable(0x003A, kernel_globals + offsetof(KernelGuestGlobals, IoDeviceObjectType));
  module.SetVariable(0x003E, kernel_globals + offsetof(KernelGuestGlobals, IoFileObjectType));
  module.SetVariable(0x0106, kernel_globals + offsetof(KernelGuestGlobals, ObDirectoryObjectType));
  module.SetVariable(0x0112,
                     kernel_globals + offsetof(KernelGuestGlobals, ObSymbolicLinkObjectType));
  module.SetVariable(0x02DB,
                     kernel_globals + offsetof(KernelGuestGlobals, UsbdBootEnumerationDoneEvent));

  return handle;
}

}  // namespace rex::kernel::xboxkrnl
