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

#include <rex/logging/log_category.h>
#include "kernel/categories.h"
#include <rex/ppc/hook.h>
#include <rex/memory/mapped_ptr.h>
#include <rex/kernel/module.h>
#include <rex/types.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/object/module_object.h>
#include <rex/kernel/types/thread.h>
#include <rex/kernel/object/kernel_thread.h>
#include <rex/kernel/types/status.h>
#include "kernel/stub_log.h"
#include "kernel/title_state.h"
#include "kernel/kernel.h"
#include <rex/kernel/types/xex.h>
#include <rex/kernel/object/object_table.h>

namespace rex::kernel::xboxkrnl {
using namespace rex::kernel;

/** Privilege 6 selects the flag 0x40 (kXEXSystemInsecureSockets). */
u32 XexCheckExecutablePrivilege_entry(u32 privilege) {
  REXKRNL_TRACE(
      "XexCheckExecutablePrivilege "
      "priv={}",
      static_cast<u32>(privilege));
  u32 mask = 1 << privilege;

  auto module = kernel::GetCurrent().GetObjectTable().Get<ModuleObject>(
      kernel::GetCurrent().GetModuleRegistry().GetExecutable().value_or(0));
  if (!module) {
    return 0;
  }

  u32 flags = 0;
  module->OptHeader<u32>(kXEXHeaderSystemFlags, &flags);

  return (flags & mask) > 0;
}

u32 XexGetModuleHandle_entry(MappedString module_name, MappedU32 hmodule_ptr) {
  X_HANDLE module_handle = 0;

  if (!module_name) {
    module_handle = kernel::GetCurrent().GetModuleRegistry().GetExecutable().value_or(0);
  } else {
    module_handle =
        kernel::GetCurrent().GetModuleRegistry().GetModule(module_name.GetValue()).value_or(0);
  }

  auto module = kernel::GetCurrent().GetObjectTable().Get<ModuleObject>(module_handle);
  if (!module) {
    *hmodule_ptr = 0;
    return X_ERROR_NOT_FOUND;
  }

  *hmodule_ptr = module->hmodule_ptr;

  return X_ERROR_SUCCESS;
}

u32 XexGetModuleSection_entry(MappedVoid hmodule, MappedString name, MappedU32 data_ptr,
                              MappedU32 size_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto module = kernel::GetCurrent().GetObjectTable().Get<ModuleObject>(
      ModuleObject::FromHModule(kernel::GetCurrent(), hmodule));
  if (module) {
    u32 section_data = 0;
    u32 section_size = 0;
    result = module->GetSection(name.GetValue(), &section_data, &section_size);
    if (XSUCCEEDED(result)) {
      *data_ptr = section_data;
      *size_ptr = section_size;
    }
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}

u32 XexLoadImage_entry(MappedString module_name, [[maybe_unused]] u32 module_flags,
                       [[maybe_unused]] u32 min_version, MappedU32 hmodule_ptr) {
  X_STATUS result = X_STATUS_UNSUCCESSFUL;

  auto& runtime = kernel::GetCurrent();
  auto& modules = runtime.GetModuleRegistry();
  u32 hmodule = 0;
  {
    std::lock_guard lock(modules.GetLoadCountMutex());
    auto module = runtime.GetObjectTable().Get<ModuleObject>(
        modules.GetModule(module_name.GetValue()).value_or(0));
    if (module) {
      hmodule = module->hmodule_ptr;
      auto ldr_data =
          StateOf(runtime).GetAddressSpace()->TranslateVirtual<X_LDR_DATA_TABLE_ENTRY*>(hmodule);
      ldr_data->load_count++;
      result = X_STATUS_SUCCESS;
    }
  }

  if (!hmodule) {
    auto loaded = modules.LoadUserModule(module_name.GetValue());
    auto user_module =
        runtime.GetObjectTable().Get<ModuleObject>(loaded ? *loaded : static_cast<X_HANDLE>(0));
    if (user_module) {
      hmodule = user_module->hmodule_ptr;
      result = X_STATUS_SUCCESS;
    } else if (!loaded) {
      result = ToXStatus(loaded.error());
    }
  }

  *hmodule_ptr = hmodule;

  return result;
}

u32 XexUnloadImage_entry(MappedVoid hmodule) {
  const X_HANDLE module_handle = ModuleObject::FromHModule(kernel::GetCurrent(), hmodule);
  auto module = kernel::GetCurrent().GetObjectTable().Get<ModuleObject>(module_handle);
  if (!module) {
    return X_STATUS_INVALID_HANDLE;
  }
  if (module->type == ModuleType::kKernel) {
    return X_STATUS_SUCCESS;
  }

  // Decrement and check under the lock so concurrent unloads cannot both see 0 and double free.
  bool last_ref;
  {
    std::lock_guard lock(kernel::GetCurrent().GetModuleRegistry().GetLoadCountMutex());
    auto ldr_data = hmodule.As<X_LDR_DATA_TABLE_ENTRY*>();
    last_ref = (--ldr_data->load_count == 0);
  }

  if (last_ref) {
    kernel::GetCurrent().GetModuleRegistry().UnloadUserModule(module_handle);
  }

  return X_STATUS_SUCCESS;
}

u32 XexUnloadImageAndExitThread_entry(MappedVoid hmodule, u32 exit_code) {
  XexUnloadImage_entry(hmodule);
  KernelThread::GetCallingThread()->Exit(static_cast<int>(exit_code));
}

u32 XexGetProcedureAddress_entry(MappedVoid hmodule, u32 ordinal, MappedU32 out_function_ptr) {
  REX_ASSERT(ordinal != 0);

  u32 caller_address = 0;
  auto* thread = KernelThread::GetCallingThread();
  if (thread && thread->GetThreadState() && thread->GetThreadState()->GetContext()) {
    caller_address = static_cast<u32>(thread->GetThreadState()->GetContext()->lr);
  }

  bool is_string_name = (ordinal & 0xFFFF0000) != 0;
  auto string_name = reinterpret_cast<const char*>(
      kernel::GetCurrentState().GetAddressSpace()->TranslateVirtual(ordinal));

  X_STATUS result = X_STATUS_INVALID_HANDLE;

  X_HANDLE module_handle = 0;
  if (!hmodule) {
    module_handle = kernel::GetCurrent().GetModuleRegistry().GetExecutable().value_or(0);
  } else {
    module_handle = ModuleObject::FromHModule(kernel::GetCurrent(), hmodule);
  }
  auto module = kernel::GetCurrent().GetObjectTable().Get<ModuleObject>(module_handle);
  if (module) {
    u32 ptr;
    if (is_string_name) {
      ptr = module->GetExportAddress(string_name);
    } else {
      ptr = module->GetExportAddress(kernel::GetCurrent(), ordinal, caller_address);
    }
    if (ptr) {
      *out_function_ptr = ptr;
      result = X_STATUS_SUCCESS;
    } else {
      if (is_string_name) {
        REXKRNL_WARN("ERROR: XexGetProcedureAddress export '{}' in '{}' not found!", string_name,
                     module->name);
      } else {
        REXKRNL_WARN(
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

void ExRegisterTitleTerminateNotification_entry(GuestPtrT<X_EX_TITLE_TERMINATE_REGISTRATION> reg,
                                                u32 create) {
  if (create) {
    kernel::GetCurrentState().RegisterTitleTerminateNotification(reg.GetGuestAddress());
  } else {
    kernel::GetCurrentState().RemoveTitleTerminateNotification(reg.GetGuestAddress());
  }
}

u32 XexLoadImageHeaders_entry(MappedString path, [[maybe_unused]] MappedVoid headers) {
  REX_KERNEL_STUB("XexLoadImageHeaders({})", path.GetValue());
  return X_STATUS_NOT_IMPLEMENTED;
}

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__XexCheckExecutablePrivilege,
           rex::kernel::xboxkrnl::XexCheckExecutablePrivilege_entry)
REX_EXPORT(__imp__XexGetModuleHandle, rex::kernel::xboxkrnl::XexGetModuleHandle_entry)
REX_EXPORT(__imp__XexGetModuleSection, rex::kernel::xboxkrnl::XexGetModuleSection_entry)
REX_EXPORT(__imp__XexLoadImage, rex::kernel::xboxkrnl::XexLoadImage_entry)
REX_EXPORT(__imp__XexUnloadImage, rex::kernel::xboxkrnl::XexUnloadImage_entry)
REX_EXPORT(__imp__XexUnloadImageAndExitThread,
           rex::kernel::xboxkrnl::XexUnloadImageAndExitThread_entry)
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
