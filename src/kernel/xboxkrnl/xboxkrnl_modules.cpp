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
#include <rex/ppc/function.h>
#include <rex/ppc/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xexception.h>
#include <rex/system/xtypes.h>

namespace rex::kernel::xboxkrnl {
using namespace rex::system;

ppc_u32_result_t XexCheckExecutablePrivilege_entry(ppc_u32_t privilege) {
  REXKRNL_IMPORT_TRACE("XexCheckExecutablePrivilege", "priv={}", (uint32_t)privilege);
  // BOOL
  // DWORD Privilege

  // Privilege is bit position in xe_xex2_system_flags enum - so:
  // Privilege=6 -> 0x00000040 -> XEX_SYSTEM_INSECURE_SOCKETS
  uint32_t mask = 1 << privilege;

  auto module = kernel_state()->GetExecutableModule();
  if (!module) {
    return 0;
  }

  uint32_t flags = 0;
  module->GetOptHeader<uint32_t>(XEX_HEADER_SYSTEM_FLAGS, &flags);

  return (flags & mask) > 0;
}

ppc_u32_result_t XexGetModuleHandle_entry(ppc_pchar_t module_name, ppc_pu32_t hmodule_ptr) {
  object_ref<XModule> module;

  if (!module_name) {
    module = kernel_state()->GetExecutableModule();
  } else {
    module = kernel_state()->GetModule(module_name.value());
  }

  if (!module) {
    *hmodule_ptr = 0;
    return X_ERROR_NOT_FOUND;
  }

  // NOTE: we don't retain the handle for return.
  *hmodule_ptr = module->hmodule_ptr();

  return X_ERROR_SUCCESS;
}

ppc_u32_result_t XexGetModuleSection_entry(ppc_pvoid_t hmodule, ppc_pchar_t name,
                                           ppc_pu32_t data_ptr, ppc_pu32_t size_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto module = XModule::GetFromHModule(kernel_state(), hmodule);
  if (module) {
    uint32_t section_data = 0;
    uint32_t section_size = 0;
    result = module->GetSection(name.value(), &section_data, &section_size);
    if (XSUCCEEDED(result)) {
      *data_ptr = section_data;
      *size_ptr = section_size;
    }
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}

ppc_u32_result_t XexLoadImage_entry(ppc_pchar_t module_name, ppc_u32_t module_flags,
                                    ppc_u32_t min_version, ppc_pu32_t hmodule_ptr) {
  X_STATUS result = X_STATUS_NO_SUCH_FILE;

  uint32_t hmodule = 0;
  auto module = kernel_state()->GetModule(module_name.value());
  if (module) {
    // Existing module found.
    hmodule = module->hmodule_ptr();
    result = X_STATUS_SUCCESS;
  } else {
    // Not found; attempt to load as a user module.
    auto user_module = kernel_state()->LoadUserModule(module_name.value());
    if (user_module) {
      // Give up object ownership, this reference will be released by the last
      // XexUnloadImage call
      auto user_module_raw = user_module.release();
      hmodule = user_module_raw->hmodule_ptr();
      result = X_STATUS_SUCCESS;
    }
  }

  // Increment the module's load count.
  if (hmodule) {
    auto ldr_data = kernel_memory()->TranslateVirtual<X_LDR_DATA_TABLE_ENTRY*>(hmodule);
    ldr_data->load_count++;
  }

  *hmodule_ptr = hmodule;

  return result;
}

ppc_u32_result_t XexUnloadImage_entry(ppc_pvoid_t hmodule) {
  auto module = XModule::GetFromHModule(kernel_state(), hmodule);
  if (!module) {
    return X_STATUS_INVALID_HANDLE;
  }

  // Can't unload kernel modules from user code.
  if (module->module_type() != XModule::ModuleType::kKernelModule) {
    auto ldr_data = hmodule.as<X_LDR_DATA_TABLE_ENTRY*>();
    if (--ldr_data->load_count == 0) {
      // No more references, free it.
      module->Release();
      kernel_state()->UnloadUserModule(
          object_ref<UserModule>(reinterpret_cast<UserModule*>(module.release())));
    }
  }

  return X_STATUS_SUCCESS;
}

ppc_u32_result_t XexGetProcedureAddress_entry(ppc_pvoid_t hmodule, ppc_u32_t ordinal,
                                              ppc_pu32_t out_function_ptr) {
  // May be entry point?
  assert_not_zero(ordinal);

  bool is_string_name = (ordinal & 0xFFFF0000) != 0;
  auto string_name = reinterpret_cast<const char*>(kernel_memory()->TranslateVirtual(ordinal));

  X_STATUS result = X_STATUS_INVALID_HANDLE;

  object_ref<XModule> module;
  if (!hmodule) {
    module = kernel_state()->GetExecutableModule();
  } else {
    module = XModule::GetFromHModule(kernel_state(), hmodule);
  }
  if (module) {
    uint32_t ptr;
    if (is_string_name) {
      ptr = module->GetProcAddressByName(string_name);
    } else {
      ptr = module->GetProcAddressByOrdinal(ordinal);
    }
    if (ptr) {
      *out_function_ptr = ptr;
      result = X_STATUS_SUCCESS;
    } else {
      if (is_string_name) {
        REXKRNL_WARN("ERROR: XexGetProcedureAddress export '{}' in '{}' not found!", string_name,
                     module->name());
      } else {
        REXKRNL_WARN(
            "ERROR: XexGetProcedureAddress ordinal {} (0x{:X}) in '{}' not "
            "found!",
            ordinal, ordinal, module->name());
      }
      *out_function_ptr = 0;
      result = X_STATUS_DRIVER_ENTRYPOINT_NOT_FOUND;
    }
  }

  return result;
}

void ExRegisterTitleTerminateNotification_entry(ppc_ptr_t<X_EX_TITLE_TERMINATE_REGISTRATION> reg,
                                                ppc_u32_t create) {
  if (create) {
    // Adding.
    kernel_state()->RegisterTitleTerminateNotification(reg->notification_routine, reg->priority);
  } else {
    // Removing.
    kernel_state()->RemoveTitleTerminateNotification(reg->notification_routine);
  }
}

ppc_u32_result_t XexLoadImageHeaders_entry(ppc_pchar_t path, ppc_pvoid_t headers) {
  REXKRNL_DEBUG("XexLoadImageHeaders({}) - stub", path.value());
  return X_STATUS_NOT_IMPLEMENTED;
}

}  // namespace rex::kernel::xboxkrnl

PPC_HOOK(__imp__XexCheckExecutablePrivilege,
         rex::kernel::xboxkrnl::XexCheckExecutablePrivilege_entry)
PPC_HOOK(__imp__XexGetModuleHandle, rex::kernel::xboxkrnl::XexGetModuleHandle_entry)
PPC_HOOK(__imp__XexGetModuleSection, rex::kernel::xboxkrnl::XexGetModuleSection_entry)
PPC_HOOK(__imp__XexLoadImage, rex::kernel::xboxkrnl::XexLoadImage_entry)
PPC_HOOK(__imp__XexUnloadImage, rex::kernel::xboxkrnl::XexUnloadImage_entry)
PPC_HOOK(__imp__XexGetProcedureAddress, rex::kernel::xboxkrnl::XexGetProcedureAddress_entry)
PPC_HOOK(__imp__ExRegisterTitleTerminateNotification,
         rex::kernel::xboxkrnl::ExRegisterTitleTerminateNotification_entry)
PPC_HOOK(__imp__XexLoadImageHeaders, rex::kernel::xboxkrnl::XexLoadImageHeaders_entry)
