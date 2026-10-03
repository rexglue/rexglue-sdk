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

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <cstring>
#include <memory>
#include <vector>

#include <fmt/format.h>

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
#include <rex/runtime/user_module.h>
#include <rex/runtime/xthread.h>
#include "runtime/cvar_table.h"

namespace rex::kernel::xboxkrnl {
using namespace rex::runtime;

X_HANDLE CreateXboxkrnlModule(Runtime& runtime) {
  const X_HANDLE handle = CreateKernelModule(runtime, "xe:\\xboxkrnl.exe");
  if (!handle) {
    return 0;
  }
  auto module_pin = runtime.object_table().Get<ModuleObject>(handle);
  ModuleObject& module = *module_pin;
  auto* mem = runtime.memory();

  // Register video variable exports (VdGlobalDevice, VdGpuClockInMHz, etc.)
  RegisterVideoExports(module, runtime);

  for (const u32 ordinal : {0x0059u, 0x0266u}) {
    const u32 monitor_data = mem->SystemHeapAlloc(4);
    memory::store_and_swap<u32>(mem->TranslateVirtual(monitor_data), 0);
    KernelModuleSetVariable(module, ordinal, monitor_data);
  }

  // XboxHardwareInfo (XboxHardwareInfo_t, 16b)
  // flags       cpu#  ?     ?     ?     ?           ?       ?
  // 0x00000000, 0x06, 0x00, 0x00, 0x00, 0x00000000, 0x0000, 0x0000
  // Games seem to check if bit 26 (0x20) is set, which at least for xbox1
  // was whether an HDD was present. Not sure what the other flags are.
  //
  // aomega08 says the value is 0x02000817, bit 27: debug mode on.
  // When that is set, though, allocs crash in weird ways.
  //
  // From kernel dissasembly, after storage is initialized
  // XboxHardwareInfo flags is set with flag 5 (0x20).
  uint32_t pXboxHardwareInfo = mem->SystemHeapAlloc(16);
  auto lpXboxHardwareInfo = mem->TranslateVirtual(pXboxHardwareInfo);
  KernelModuleSetVariable(module, 0x0156, pXboxHardwareInfo);
  memory::store_and_swap<uint32_t>(lpXboxHardwareInfo + 0, 0x20);  // flags
  memory::store_and_swap<uint8_t>(lpXboxHardwareInfo + 4, 0x06);   // cpu count
  // Remaining 11b are zeroes?

  // ExConsoleGameRegion, probably same values as keyvault region uses?
  // Just return all 0xFF, should satisfy anything that checks it
  uint32_t pExConsoleGameRegion = mem->SystemHeapAlloc(4);
  auto lpExConsoleGameRegion = mem->TranslateVirtual(pExConsoleGameRegion);
  KernelModuleSetVariable(module, 0x000C, pExConsoleGameRegion);
  memory::store<uint32_t>(lpExConsoleGameRegion, 0xFFFFFFFF);

  // XexExecutableModuleHandle (?**)
  // Games try to dereference this to get a pointer to some module struct.
  // So far it seems like it's just in loader code, and only used to look up
  // the XexHeaderBase for use by RtlImageXexHeaderField.
  // We fake it so that the address passed to that looks legit.
  // 0x80100FFC <- pointer to structure
  // 0x80101000 <- our module structure
  // 0x80101058 <- pointer to xex header
  // 0x80101100 <- xex header base
  uint32_t ppXexExecutableModuleHandle = mem->SystemHeapAlloc(4);
  KernelModuleSetVariable(module, 0x0193, ppXexExecutableModuleHandle);

  // ExLoadedImageName (char*)
  // The full path to loaded image/xex including its name.
  // Used usually in custom dashboards (Aurora)
  // Todo(Gliniak): Confirm that official kernel always allocate space for this
  // variable.
  uint32_t ppExLoadedImageName = mem->SystemHeapAlloc(kExLoadedImageNameSize);
  KernelModuleSetVariable(module, 0x01AF, ppExLoadedImageName);

  // ExLoadedCommandLine (char*)
  // The name of the xex. Not sure this is ever really used on real devices.
  // Perhaps it's how swap disc/etc data is sent?
  // Always set to "default.xex" (with quotes) for now.
  // TODO(gibbed): set this to the actual module name.
  std::string command_line("\"default.xex\"");
  if (!runtime.config().runtime.kernel.command_line.empty()) {
    command_line += " " + runtime.config().runtime.kernel.command_line;
  }
  uint32_t command_line_length =
      rex::align(static_cast<uint32_t>(command_line.length()) + 1, 1024u);
  uint32_t pExLoadedCommandLine = mem->SystemHeapAlloc(command_line_length);
  auto lpExLoadedCommandLine = mem->TranslateVirtual(pExLoadedCommandLine);
  KernelModuleSetVariable(module, 0x01AE, pExLoadedCommandLine);
  std::memset(lpExLoadedCommandLine, 0, command_line_length);
  std::memcpy(lpExLoadedCommandLine, command_line.c_str(), command_line.length());

  // XboxKrnlVersion (8b)
  // Kernel version, looks like 2b.2b.2b.2b.
  // I've only seen games check >=, so we just fake something here.
  uint32_t pXboxKrnlVersion = mem->SystemHeapAlloc(8);
  auto lpXboxKrnlVersion = mem->TranslateVirtual(pXboxKrnlVersion);
  KernelModuleSetVariable(module, 0x0158, pXboxKrnlVersion);
  memory::store_and_swap<uint16_t>(lpXboxKrnlVersion + 0, 2);
  memory::store_and_swap<uint16_t>(lpXboxKrnlVersion + 2, 0xFFFF);
  memory::store_and_swap<uint16_t>(lpXboxKrnlVersion + 4, 0xFFFF);
  memory::store_and_swap<uint8_t>(lpXboxKrnlVersion + 6, 0x80);
  memory::store_and_swap<uint8_t>(lpXboxKrnlVersion + 7, 0x00);

  // KeTimeStampBundle (24b)
  // This must be updated during execution, at 1ms intevals.
  // We setup a system timer here to do that.
  uint32_t pKeTimeStampBundle = mem->SystemHeapAlloc(24);
  auto lpKeTimeStampBundle = mem->TranslateVirtual(pKeTimeStampBundle);
  KernelModuleSetVariable(module, 0x00AD, pKeTimeStampBundle);
  memory::store_and_swap<uint64_t>(lpKeTimeStampBundle + 0, 0);
  memory::store_and_swap<uint64_t>(lpKeTimeStampBundle + 8, 0);
  memory::store_and_swap<uint32_t>(lpKeTimeStampBundle + 16, Clock::QueryGuestUptimeMillis());
  memory::store_and_swap<uint32_t>(lpKeTimeStampBundle + 20, 0);
  module.kernel.timestamp_timer = rex::platform::HighResolutionTimer::CreateRepeating(
      std::chrono::milliseconds(1), [lpKeTimeStampBundle]() {
        memory::store_and_swap<uint32_t>(lpKeTimeStampBundle + 16, Clock::QueryGuestUptimeMillis());
      });

  // Wire kernel object type variables to KernelGuestGlobals.
  auto kgg = runtime.GetKernelGuestGlobals();
  assert_not_zero(kgg);
  KernelModuleSetVariable(module, 0x001B, kgg + offsetof(KernelGuestGlobals, ExThreadObjectType));
  KernelModuleSetVariable(module, 0x000E, kgg + offsetof(KernelGuestGlobals, ExEventObjectType));
  KernelModuleSetVariable(module, 0x0012, kgg + offsetof(KernelGuestGlobals, ExMutantObjectType));
  KernelModuleSetVariable(module, 0x0017,
                          kgg + offsetof(KernelGuestGlobals, ExSemaphoreObjectType));
  KernelModuleSetVariable(module, 0x001C, kgg + offsetof(KernelGuestGlobals, ExTimerObjectType));
  KernelModuleSetVariable(module, 0x0036,
                          kgg + offsetof(KernelGuestGlobals, IoCompletionObjectType));
  KernelModuleSetVariable(module, 0x003A, kgg + offsetof(KernelGuestGlobals, IoDeviceObjectType));
  KernelModuleSetVariable(module, 0x003E, kgg + offsetof(KernelGuestGlobals, IoFileObjectType));
  KernelModuleSetVariable(module, 0x0106,
                          kgg + offsetof(KernelGuestGlobals, ObDirectoryObjectType));
  KernelModuleSetVariable(module, 0x0112,
                          kgg + offsetof(KernelGuestGlobals, ObSymbolicLinkObjectType));
  KernelModuleSetVariable(module, 0x02DB,
                          kgg + offsetof(KernelGuestGlobals, UsbdBootEnumerationDoneEvent));

  return handle;
}

}  // namespace rex::kernel::xboxkrnl
