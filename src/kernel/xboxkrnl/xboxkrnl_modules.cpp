/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/kernel/xboxkrnl/private.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/runtime.h>
#include <rex/runtime/user_module.h>
#include <rex/runtime/xexception.h>
#include <rex/runtime/xthread.h>
#include <rex/runtime/xtypes.h>
#include "kernel/kernel_macros.h"

namespace rex::kernel::xboxkrnl {
using namespace rex::runtime;

/**
 * Reports whether the executable has a system flag set.
 *
 * The privilege is the bit index, so 6 selects 0x40 (XEX_SYSTEM_INSECURE_SOCKETS).
 *
 * @param privilege Bit position in the XEX system flags.
 * @return 1 when the flag is set, otherwise 0.
 */
u32 XexCheckExecutablePrivilege_entry(u32 privilege) {
  REXLOG_TRACE(kernel,
               "XexCheckExecutablePrivilege "
               "priv={}",
               static_cast<uint32_t>(privilege));
  uint32_t mask = 1 << privilege;

  auto module = runtime::Current().object_table().Get<ModuleObject>(
      runtime::Current().module_registry().executable().value_or(0));
  if (!module) {
    return 0;
  }

  uint32_t flags = 0;
  UserModuleGetOptHeader<uint32_t>(*module, XEX_HEADER_SYSTEM_FLAGS, &flags);

  return (flags & mask) > 0;
}

u32 XexGetModuleHandle_entry(mapped_string module_name, mapped_u32 hmodule_ptr) {
  X_HANDLE module_handle = 0;

  if (!module_name) {
    module_handle = runtime::Current().module_registry().executable().value_or(0);
  } else {
    module_handle = runtime::Current().module_registry().GetModule(module_name.value()).value_or(0);
  }

  auto module = runtime::Current().object_table().Get<ModuleObject>(module_handle);
  if (!module) {
    *hmodule_ptr = 0;
    return X_ERROR_NOT_FOUND;
  }

  *hmodule_ptr = module->hmodule_ptr;

  return X_ERROR_SUCCESS;
}

u32 XexGetModuleSection_entry(mapped_void hmodule, mapped_string name, mapped_u32 data_ptr,
                              mapped_u32 size_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto module = runtime::Current().object_table().Get<ModuleObject>(
      ModuleFromHModule(runtime::Current(), hmodule));
  if (module) {
    uint32_t section_data = 0;
    uint32_t section_size = 0;
    result = ModuleGetSection(*module, name.value(), &section_data, &section_size);
    if (XSUCCEEDED(result)) {
      *data_ptr = section_data;
      *size_ptr = section_size;
    }
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}

u32 XexLoadImage_entry(mapped_string module_name, [[maybe_unused]] u32 module_flags,
                       [[maybe_unused]] u32 min_version, mapped_u32 hmodule_ptr) {
  X_STATUS result = X_STATUS_NO_SUCH_FILE;

  auto& runtime = runtime::Current();
  auto& modules = runtime.module_registry();
  uint32_t hmodule = 0;
  {
    std::lock_guard lock(modules.load_count_mutex());
    auto module = runtime.object_table().Get<ModuleObject>(
        modules.GetModule(module_name.value()).value_or(0));
    if (module) {
      hmodule = module->hmodule_ptr;
      auto ldr_data = runtime.memory()->TranslateVirtual<X_LDR_DATA_TABLE_ENTRY*>(hmodule);
      ldr_data->load_count++;
      result = X_STATUS_SUCCESS;
    }
  }

  if (!hmodule) {
    auto loaded = modules.LoadUserModule(module_name.value());
    auto user_module = runtime.object_table().Get<ModuleObject>(loaded ? *loaded : X_HANDLE(0));
    if (user_module) {
      hmodule = user_module->hmodule_ptr;
      std::lock_guard lock(modules.load_count_mutex());
      auto ldr_data = runtime.memory()->TranslateVirtual<X_LDR_DATA_TABLE_ENTRY*>(hmodule);
      ldr_data->load_count++;
      result = X_STATUS_SUCCESS;
    }
  }

  *hmodule_ptr = hmodule;

  return result;
}

u32 XexUnloadImage_entry(mapped_void hmodule) {
  const X_HANDLE module_handle = ModuleFromHModule(runtime::Current(), hmodule);
  auto module = runtime::Current().object_table().Get<ModuleObject>(module_handle);
  if (!module) {
    return X_STATUS_INVALID_HANDLE;
  }
  if (module->type == ModuleType::Kernel) {
    return X_STATUS_SUCCESS;
  }

  // Decrement and check under the lock so concurrent unloads cannot both see 0 and double free.
  bool last_ref;
  {
    std::lock_guard lock(runtime::Current().module_registry().load_count_mutex());
    auto ldr_data = hmodule.as<X_LDR_DATA_TABLE_ENTRY*>();
    last_ref = (--ldr_data->load_count == 0);
  }

  if (last_ref) {
    runtime::Current().module_registry().UnloadUserModule(module_handle);
  }

  return X_STATUS_SUCCESS;
}

u32 XexGetProcedureAddress_entry(mapped_void hmodule, u32 ordinal, mapped_u32 out_function_ptr) {
  REX_ASSERT(ordinal != 0);

  uint32_t caller_address = 0;
  auto* thread = ThreadRecord::GetCurrentThread();
  if (thread && thread->thread_state() && thread->thread_state()->context()) {
    caller_address = static_cast<uint32_t>(thread->thread_state()->context()->lr);
  }

  bool is_string_name = (ordinal & 0xFFFF0000) != 0;
  auto string_name =
      reinterpret_cast<const char*>(runtime::Current().memory()->TranslateVirtual(ordinal));

  X_STATUS result = X_STATUS_INVALID_HANDLE;

  X_HANDLE module_handle = 0;
  if (!hmodule) {
    module_handle = runtime::Current().module_registry().executable().value_or(0);
  } else {
    module_handle = ModuleFromHModule(runtime::Current(), hmodule);
  }
  auto module = runtime::Current().object_table().Get<ModuleObject>(module_handle);
  if (module) {
    uint32_t ptr;
    if (is_string_name) {
      ptr = ModuleGetProcAddressByName(*module, string_name);
    } else {
      ptr = ModuleGetProcAddressByOrdinal(runtime::Current(), *module, ordinal, caller_address);
    }
    if (ptr) {
      *out_function_ptr = ptr;
      result = X_STATUS_SUCCESS;
    } else {
      if (is_string_name) {
        REXLOG_WARN(kernel, "ERROR: XexGetProcedureAddress export '{}' in '{}' not found!",
                    string_name, module->name);
      } else {
        REXLOG_WARN(kernel,
                    "ERROR: XexGetProcedureAddress ordinal {} (0x{:X}) in '{}' not "
                    "found!",
                    ordinal, ordinal, module->name);
      }
      *out_function_ptr = 0;
      result = X_STATUS_DRIVER_ENTRYPOINT_NOT_FOUND;
    }
  }

  return result;
}

void ExRegisterTitleTerminateNotification_entry(ppc_ptr_t<X_EX_TITLE_TERMINATE_REGISTRATION> reg,
                                                u32 create) {
  if (create) {
    runtime::Current().RegisterTitleTerminateNotification(reg.guest_address());
  } else {
    runtime::Current().RemoveTitleTerminateNotification(reg.guest_address());
  }
}

u32 XexLoadImageHeaders_entry(mapped_string path, [[maybe_unused]] mapped_void headers) {
  REX_KERNEL_STUB("XexLoadImageHeaders({})", path.value());
  return X_STATUS_NOT_IMPLEMENTED;
}

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__XexCheckExecutablePrivilege,
           rex::kernel::xboxkrnl::XexCheckExecutablePrivilege_entry)
REX_EXPORT(__imp__XexGetModuleHandle, rex::kernel::xboxkrnl::XexGetModuleHandle_entry)
REX_EXPORT(__imp__XexGetModuleSection, rex::kernel::xboxkrnl::XexGetModuleSection_entry)
REX_EXPORT(__imp__XexLoadImage, rex::kernel::xboxkrnl::XexLoadImage_entry)
REX_EXPORT(__imp__XexUnloadImage, rex::kernel::xboxkrnl::XexUnloadImage_entry)
REX_EXPORT(__imp__XexGetProcedureAddress, rex::kernel::xboxkrnl::XexGetProcedureAddress_entry)
REX_EXPORT(__imp__ExRegisterTitleTerminateNotification,
           rex::kernel::xboxkrnl::ExRegisterTitleTerminateNotification_entry)
REX_EXPORT(__imp__XexLoadImageHeaders, rex::kernel::xboxkrnl::XexLoadImageHeaders_entry)

REX_EXPORT_STUB(__imp__XexLoadExecutable);
REX_EXPORT_STUB(__imp__XexLoadImageFromMemory);
REX_EXPORT_STUB(__imp__XexPcToFileHeader);
REX_EXPORT_STUB(__imp__XexRegisterPatchDescriptor);
REX_EXPORT_STUB(__imp__XexSendDeferredNotifications);
REX_EXPORT_STUB(__imp__XexStartExecutable);
REX_EXPORT_STUB(__imp__XexUnloadImageAndExitThread);
REX_EXPORT_STUB(__imp__XexUnloadTitleModules);
REX_EXPORT_STUB(__imp__XexVerifyImageHeaders);
REX_EXPORT_STUB(__imp__XexGetModuleImportVersions);
REX_EXPORT_STUB(__imp__XexActivationGetNonce);
REX_EXPORT_STUB(__imp__XexActivationSetLicense);
REX_EXPORT_STUB(__imp__XexActivationVerifyOwnership);
REX_EXPORT_STUB(__imp__XexDisableVerboseDbgPrint);
REX_EXPORT_STUB(__imp__XexImportTraceEnable);
REX_EXPORT_STUB(__imp__XexSetExecutablePrivilege);
REX_EXPORT_STUB(__imp__XexSetLastKdcTime);
REX_EXPORT_STUB(__imp__XexTransformImageKey);
REX_EXPORT_STUB(__imp__XexShimDisable);
REX_EXPORT_STUB(__imp__XexShimEnable);
REX_EXPORT_STUB(__imp__XexShimEntryDisable);
REX_EXPORT_STUB(__imp__XexShimEntryEnable);
REX_EXPORT_STUB(__imp__XexShimEntryRegister);
REX_EXPORT_STUB(__imp__XexShimLock);
REX_EXPORT_STUB(__imp__XexTitleHash);
REX_EXPORT_STUB(__imp__XexTitleHashClose);
REX_EXPORT_STUB(__imp__XexTitleHashContinue);
REX_EXPORT_STUB(__imp__XexTitleHashOpen);
REX_EXPORT_STUB(__imp__XexReserveCodeBuffer);
REX_EXPORT_STUB(__imp__XexCommitCodeBuffer);
REX_EXPORT_STUB(__imp__XexRegisterUsermodeModule);
REX_EXPORT_STUB(__imp__LDICreateDecompression);
REX_EXPORT_STUB(__imp__LDIDecompress);
REX_EXPORT_STUB(__imp__LDIDestroyDecompression);
REX_EXPORT_STUB(__imp__LDIResetDecompression);
