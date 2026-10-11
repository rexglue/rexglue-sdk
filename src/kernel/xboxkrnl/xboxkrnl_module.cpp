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

#include <rex/api.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/kernel/xboxkrnl/module.h>
#include <rex/kernel/xboxkrnl/private.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ppc/context.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/runtime.h>
#include <rex/runtime/module_object.h>
#include <rex/runtime/xthread.h>
#include "runtime/cvar_table.h"

namespace rex::kernel::xboxkrnl {
using namespace rex::runtime;

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

X_HANDLE CreateXboxkrnlModule(Runtime& runtime) {
  const X_HANDLE handle = CreateKernelModule(runtime, "xe:\\xboxkrnl.exe");
  if (!handle) {
    return 0;
  }
  auto module_pin = runtime.object_table().Get<ModuleObject>(handle);
  ModuleObject& module = *module_pin;
  auto* mem = runtime.memory();

  RegisterVideoExports(module, runtime);

  for (const u32 ordinal : {0x0059u, 0x0266u}) {
    const u32 monitor_data = mem->SystemHeapAlloc(4);
    *mem->TranslateVirtual<be_u32*>(monitor_data) = 0;
    module.SetVariable(ordinal, monitor_data);
  }

  // Titles test bit 5 (0x20) of flags. Bit 27 (debug mode) breaks allocations when set.
  u32 pXboxHardwareInfo = mem->SystemHeapAlloc(sizeof(X_HARDWARE_INFO));
  auto* hardware_info = mem->TranslateVirtual<X_HARDWARE_INFO*>(pXboxHardwareInfo);
  module.SetVariable(0x0156, pXboxHardwareInfo);
  hardware_info->flags = 0x20;
  hardware_info->cpu_count = 0x06;

  // All 0xFF satisfies any region check.
  u32 pExConsoleGameRegion = mem->SystemHeapAlloc(4);
  auto lpExConsoleGameRegion = mem->TranslateVirtual(pExConsoleGameRegion);
  module.SetVariable(0x000C, pExConsoleGameRegion);
  memory::store<u32>(lpExConsoleGameRegion, 0xFFFFFFFF);

  // Titles dereference this to find the XexHeaderBase used by RtlImageXexHeaderField.
  u32 ppXexExecutableModuleHandle = mem->SystemHeapAlloc(4);
  module.SetVariable(0x0193, ppXexExecutableModuleHandle);

  // The full path of the loaded image including its name.
  u32 ppExLoadedImageName = mem->SystemHeapAlloc(kExLoadedImageNameSize);
  module.SetVariable(0x01AF, ppExLoadedImageName);

  // The quoted xex name followed by the command line.
  std::string command_line("\"default.xex\"");
  if (!runtime.config().runtime.kernel.command_line.empty()) {
    command_line += " " + runtime.config().runtime.kernel.command_line;
  }
  u32 command_line_length = rex::align(static_cast<u32>(command_line.length()) + 1, 1024u);
  u32 pExLoadedCommandLine = mem->SystemHeapAlloc(command_line_length);
  auto lpExLoadedCommandLine = mem->TranslateVirtual(pExLoadedCommandLine);
  module.SetVariable(0x01AE, pExLoadedCommandLine);
  std::memset(lpExLoadedCommandLine, 0, command_line_length);
  std::memcpy(lpExLoadedCommandLine, command_line.c_str(), command_line.length());

  // Titles only compare the version with >=.
  u32 pXboxKrnlVersion = mem->SystemHeapAlloc(sizeof(X_KERNEL_VERSION));
  auto* kernel_version = mem->TranslateVirtual<X_KERNEL_VERSION*>(pXboxKrnlVersion);
  module.SetVariable(0x0158, pXboxKrnlVersion);
  kernel_version->major = 2;
  kernel_version->minor = 0xFFFF;
  kernel_version->build = 0xFFFF;
  kernel_version->qfe = 0x8000;

  // Updated at 1 ms intervals by a system timer.
  u32 pKeTimeStampBundle = mem->SystemHeapAlloc(sizeof(X_KE_TIMESTAMP_BUNDLE));
  auto* timestamp_bundle = mem->TranslateVirtual<X_KE_TIMESTAMP_BUNDLE*>(pKeTimeStampBundle);
  module.SetVariable(0x00AD, pKeTimeStampBundle);
  timestamp_bundle->interrupt_time = 0;
  timestamp_bundle->system_time = 0;
  timestamp_bundle->tick_count = Clock::QueryGuestUptimeMillis();
  timestamp_bundle->reserved = 0;
  module.kernel.timestamp_timer = rex::platform::HighResolutionTimer::CreateRepeating(
      std::chrono::milliseconds(1),
      [timestamp_bundle]() { timestamp_bundle->tick_count = Clock::QueryGuestUptimeMillis(); });

  auto kgg = runtime.kernel_globals();
  REX_ASSERT(kgg != 0);
  module.SetVariable(0x001B, kgg + offsetof(KernelGuestGlobals, ExThreadObjectType));
  module.SetVariable(0x000E, kgg + offsetof(KernelGuestGlobals, ExEventObjectType));
  module.SetVariable(0x0012, kgg + offsetof(KernelGuestGlobals, ExMutantObjectType));
  module.SetVariable(0x0017, kgg + offsetof(KernelGuestGlobals, ExSemaphoreObjectType));
  module.SetVariable(0x001C, kgg + offsetof(KernelGuestGlobals, ExTimerObjectType));
  module.SetVariable(0x0036, kgg + offsetof(KernelGuestGlobals, IoCompletionObjectType));
  module.SetVariable(0x003A, kgg + offsetof(KernelGuestGlobals, IoDeviceObjectType));
  module.SetVariable(0x003E, kgg + offsetof(KernelGuestGlobals, IoFileObjectType));
  module.SetVariable(0x0106, kgg + offsetof(KernelGuestGlobals, ObDirectoryObjectType));
  module.SetVariable(0x0112, kgg + offsetof(KernelGuestGlobals, ObSymbolicLinkObjectType));
  module.SetVariable(0x02DB, kgg + offsetof(KernelGuestGlobals, UsbdBootEnumerationDoneEvent));

  return handle;
}

}  // namespace rex::kernel::xboxkrnl
