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

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/kernel/xboxkrnl/cert_monitor.h>
#include <rex/kernel/xboxkrnl/debug_monitor.h>
#include <rex/kernel/xboxkrnl/module.h>
#include <rex/kernel/xboxkrnl/private.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ppc/context.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xthread.h>

REXCVAR_DEFINE_BOOL(log_high_frequency_kernel_calls, false, "Kernel",
                    "Log kernel calls with the kHighFrequency tag");

REXCVAR_DEFINE_STRING(cl, "", "Kernel", "Specify additional command-line provided to guest");

REXCVAR_DEFINE_BOOL(kernel_debug_monitor, false, "Kernel", "Enable debug monitor");

REXCVAR_DEFINE_BOOL(kernel_cert_monitor, false, "Kernel", "Enable cert monitor");

namespace rex::kernel::xboxkrnl {
using namespace rex::system;

bool XboxkrnlModule::SendPIXCommand(const char* cmd) {
  // TODO: JIT - PIX commands require JIT processor->Execute
  // if (!REXCVAR_GET(kernel_pix)) {
  //   return false;
  // }
  // ...
  (void)cmd;
  return false;
}

XboxkrnlModule::XboxkrnlModule(Runtime* emulator, KernelState* kernel_state)
    : KernelModule(kernel_state, "xe:\\xboxkrnl.exe"), timestamp_timer_(nullptr) {
  RegisterExportTable(export_resolver_);

  // Register video variable exports (VdGlobalDevice, VdGpuClockInMHz, etc.)
  RegisterVideoExports(export_resolver_, kernel_state_);

  // KeDebugMonitorData (?*)
  // Set to a valid value when a remote debugger is attached.
  // Offset 0x18 is a 4b pointer to a handler function that seems to take two
  // arguments. If we wanted to see what would happen we could fake that.
  uint32_t pKeDebugMonitorData;
  if (!REXCVAR_GET(kernel_debug_monitor)) {
    pKeDebugMonitorData = memory_->SystemHeapAlloc(4);
    auto lpKeDebugMonitorData = memory_->TranslateVirtual(pKeDebugMonitorData);
    memory::store_and_swap<uint32_t>(lpKeDebugMonitorData, 0);
  } else {
    pKeDebugMonitorData = memory_->SystemHeapAlloc(4 + sizeof(X_KEDEBUGMONITORDATA));
    memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(pKeDebugMonitorData),
                                     pKeDebugMonitorData + 4);
    auto lpKeDebugMonitorData =
        memory_->TranslateVirtual<X_KEDEBUGMONITORDATA*>(pKeDebugMonitorData + 4);
    std::memset(lpKeDebugMonitorData, 0, sizeof(X_KEDEBUGMONITORDATA));
    // TODO: JIT - GenerateTrampoline requires JIT
    // lpKeDebugMonitorData->callback_fn =
    //     GenerateTrampoline("KeDebugMonitorCallback", KeDebugMonitorCallback);
  }
  export_resolver_->SetVariableMapping("xboxkrnl.exe", 0x0059, pKeDebugMonitorData);

  // KeCertMonitorData (?*)
  // Always set to zero, ignored.
  uint32_t pKeCertMonitorData;
  if (!REXCVAR_GET(kernel_cert_monitor)) {
    pKeCertMonitorData = memory_->SystemHeapAlloc(4);
    auto lpKeCertMonitorData = memory_->TranslateVirtual(pKeCertMonitorData);
    memory::store_and_swap<uint32_t>(lpKeCertMonitorData, 0);
  } else {
    pKeCertMonitorData = memory_->SystemHeapAlloc(4 + sizeof(X_KECERTMONITORDATA));
    memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(pKeCertMonitorData),
                                     pKeCertMonitorData + 4);
    auto lpKeCertMonitorData =
        memory_->TranslateVirtual<X_KECERTMONITORDATA*>(pKeCertMonitorData + 4);
    std::memset(lpKeCertMonitorData, 0, sizeof(X_KECERTMONITORDATA));
    // TODO: JIT - GenerateTrampoline requires JIT
    // lpKeCertMonitorData->callback_fn =
    //     GenerateTrampoline("KeCertMonitorCallback", KeCertMonitorCallback);
  }
  export_resolver_->SetVariableMapping("xboxkrnl.exe", 0x0266, pKeCertMonitorData);

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
  uint32_t pXboxHardwareInfo = memory_->SystemHeapAlloc(16);
  auto lpXboxHardwareInfo = memory_->TranslateVirtual(pXboxHardwareInfo);
  export_resolver_->SetVariableMapping("xboxkrnl.exe", 0x0156, pXboxHardwareInfo);
  memory::store_and_swap<uint32_t>(lpXboxHardwareInfo + 0, 0x20);  // flags
  memory::store_and_swap<uint8_t>(lpXboxHardwareInfo + 4, 0x06);   // cpu count
  // Remaining 11b are zeroes?

  // ExConsoleGameRegion, probably same values as keyvault region uses?
  // Just return all 0xFF, should satisfy anything that checks it
  uint32_t pExConsoleGameRegion = memory_->SystemHeapAlloc(4);
  auto lpExConsoleGameRegion = memory_->TranslateVirtual(pExConsoleGameRegion);
  export_resolver_->SetVariableMapping("xboxkrnl.exe", 0x000C, pExConsoleGameRegion);
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
  uint32_t ppXexExecutableModuleHandle = memory_->SystemHeapAlloc(4);
  export_resolver_->SetVariableMapping("xboxkrnl.exe", 0x0193, ppXexExecutableModuleHandle);

  // ExLoadedImageName (char*)
  // The full path to loaded image/xex including its name.
  // Used usually in custom dashboards (Aurora)
  // Todo(Gliniak): Confirm that official kernel always allocate space for this
  // variable.
  uint32_t ppExLoadedImageName = memory_->SystemHeapAlloc(kExLoadedImageNameSize);
  export_resolver_->SetVariableMapping("xboxkrnl.exe", 0x01AF, ppExLoadedImageName);

  // ExLoadedCommandLine (char*)
  // The name of the xex. Not sure this is ever really used on real devices.
  // Perhaps it's how swap disc/etc data is sent?
  // Always set to "default.xex" (with quotes) for now.
  // TODO(gibbed): set this to the actual module name.
  std::string command_line("\"default.xex\"");
  if (!REXCVAR_GET(cl).empty()) {
    command_line += " " + REXCVAR_GET(cl);
  }
  uint32_t command_line_length =
      rex::align(static_cast<uint32_t>(command_line.length()) + 1, 1024u);
  uint32_t pExLoadedCommandLine = memory_->SystemHeapAlloc(command_line_length);
  auto lpExLoadedCommandLine = memory_->TranslateVirtual(pExLoadedCommandLine);
  export_resolver_->SetVariableMapping("xboxkrnl.exe", 0x01AE, pExLoadedCommandLine);
  std::memset(lpExLoadedCommandLine, 0, command_line_length);
  std::memcpy(lpExLoadedCommandLine, command_line.c_str(), command_line.length());

  // XboxKrnlVersion (8b)
  // Kernel version, looks like 2b.2b.2b.2b.
  // I've only seen games check >=, so we just fake something here.
  uint32_t pXboxKrnlVersion = memory_->SystemHeapAlloc(8);
  auto lpXboxKrnlVersion = memory_->TranslateVirtual(pXboxKrnlVersion);
  export_resolver_->SetVariableMapping("xboxkrnl.exe", 0x0158, pXboxKrnlVersion);
  memory::store_and_swap<uint16_t>(lpXboxKrnlVersion + 0, 2);
  memory::store_and_swap<uint16_t>(lpXboxKrnlVersion + 2, 0xFFFF);
  memory::store_and_swap<uint16_t>(lpXboxKrnlVersion + 4, 0xFFFF);
  memory::store_and_swap<uint8_t>(lpXboxKrnlVersion + 6, 0x80);
  memory::store_and_swap<uint8_t>(lpXboxKrnlVersion + 7, 0x00);

  // KeTimeStampBundle (24b)
  // This must be updated during execution, at 1ms intevals.
  // We setup a system timer here to do that.
  uint32_t pKeTimeStampBundle = memory_->SystemHeapAlloc(24);
  auto lpKeTimeStampBundle = memory_->TranslateVirtual(pKeTimeStampBundle);
  export_resolver_->SetVariableMapping("xboxkrnl.exe", 0x00AD, pKeTimeStampBundle);
  memory::store_and_swap<uint64_t>(lpKeTimeStampBundle + 0, 0);
  memory::store_and_swap<uint64_t>(lpKeTimeStampBundle + 8, 0);
  memory::store_and_swap<uint32_t>(lpKeTimeStampBundle + 16,
                                   chrono::Clock::QueryGuestUptimeMillis());
  memory::store_and_swap<uint32_t>(lpKeTimeStampBundle + 20, 0);
  timestamp_timer_ = rex::thread::HighResolutionTimer::CreateRepeating(
      std::chrono::milliseconds(1), [lpKeTimeStampBundle]() {
        memory::store_and_swap<uint32_t>(lpKeTimeStampBundle + 16,
                                         chrono::Clock::QueryGuestUptimeMillis());
      });

  // Wire kernel object type variables to KernelGuestGlobals.
  // KernelGuestGlobals is allocated in KernelState ctor, which runs before this.
  auto kgg = kernel_state_->GetKernelGuestGlobals();
  assert_not_zero(kgg);
  export_resolver_->SetVariableMapping("xboxkrnl.exe", 0x001B,
                                       kgg + offsetof(KernelGuestGlobals, ExThreadObjectType));
  export_resolver_->SetVariableMapping("xboxkrnl.exe", 0x000E,
                                       kgg + offsetof(KernelGuestGlobals, ExEventObjectType));
  export_resolver_->SetVariableMapping("xboxkrnl.exe", 0x0012,
                                       kgg + offsetof(KernelGuestGlobals, ExMutantObjectType));
  export_resolver_->SetVariableMapping("xboxkrnl.exe", 0x0017,
                                       kgg + offsetof(KernelGuestGlobals, ExSemaphoreObjectType));
  export_resolver_->SetVariableMapping("xboxkrnl.exe", 0x001C,
                                       kgg + offsetof(KernelGuestGlobals, ExTimerObjectType));
  export_resolver_->SetVariableMapping("xboxkrnl.exe", 0x0036,
                                       kgg + offsetof(KernelGuestGlobals, IoCompletionObjectType));
  export_resolver_->SetVariableMapping("xboxkrnl.exe", 0x003A,
                                       kgg + offsetof(KernelGuestGlobals, IoDeviceObjectType));
  export_resolver_->SetVariableMapping("xboxkrnl.exe", 0x003E,
                                       kgg + offsetof(KernelGuestGlobals, IoFileObjectType));
  export_resolver_->SetVariableMapping("xboxkrnl.exe", 0x0106,
                                       kgg + offsetof(KernelGuestGlobals, ObDirectoryObjectType));
  export_resolver_->SetVariableMapping(
      "xboxkrnl.exe", 0x0112, kgg + offsetof(KernelGuestGlobals, ObSymbolicLinkObjectType));
  export_resolver_->SetVariableMapping(
      "xboxkrnl.exe", 0x02DB, kgg + offsetof(KernelGuestGlobals, UsbdBootEnumerationDoneEvent));
}

static auto& get_xboxkrnl_exports() {
  static std::vector<rex::runtime::Export*> xboxkrnl_exports(4096);
  return xboxkrnl_exports;
}

rex::runtime::Export* RegisterExport_xboxkrnl(rex::runtime::Export* export_entry) {
  auto& xboxkrnl_exports = get_xboxkrnl_exports();
  assert_true(export_entry->ordinal < xboxkrnl_exports.size());
  xboxkrnl_exports[export_entry->ordinal] = export_entry;
  return export_entry;
}

void XboxkrnlModule::RegisterExportTable(rex::runtime::ExportResolver* export_resolver) {
  assert_not_null(export_resolver);

// Build the export table used for resolution.
#include "../export_table_pre.inc"
  static rex::runtime::Export xboxkrnl_export_table[] = {
#include "export_table.inc"
  };
#include "../export_table_post.inc"
  auto& xboxkrnl_exports = get_xboxkrnl_exports();
  for (size_t i = 0; i < rex::countof(xboxkrnl_export_table); ++i) {
    auto& export_entry = xboxkrnl_export_table[i];
    assert_true(export_entry.ordinal < xboxkrnl_exports.size());
    if (!xboxkrnl_exports[export_entry.ordinal]) {
      xboxkrnl_exports[export_entry.ordinal] = &export_entry;
    }
  }
  export_resolver->RegisterTable("xboxkrnl.exe", &xboxkrnl_exports);
}

XboxkrnlModule::~XboxkrnlModule() = default;

}  // namespace rex::kernel::xboxkrnl
