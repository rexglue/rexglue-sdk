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

#include <rex/logging/log_category.h>
#include <rex/kernel/export.h>
#include <rex/kernel/object/object_table.h>
#include <rex/string/text.h>
#include <rex/string/path.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/object/module_object.h>
#include "kernel/title_state.h"
#include <rex/filesystem/xex.h>
#include <rex/kernel/types/xex.h>

namespace rex::kernel {

namespace {

u32 KernelProcAddressByOrdinal(TitleRuntime& runtime, ModuleObject& module, u16 ordinal,
                               u32 caller_address) {
  const auto runtime_module = rex::kernel::ModuleFromName(module.name);
  const auto* export_entry =
      runtime_module ? rex::kernel::FindByOrdinal(*runtime_module, ordinal) : nullptr;
  if (!export_entry) {
    REXSYS_DEBUG("GetProcAddressByOrdinal: ordinal {:04X} not found in {}", ordinal, module.name);
    return 0;
  }

  if (export_entry->type == rex::kernel::ExportType::kVariable) {
    const u32 address = module.GetVariable(ordinal);
    REXSYS_DEBUG("GetProcAddressByOrdinal: {} ({:04X}) -> variable at {:08X}", export_entry->name,
                 ordinal, address);
    return address;
  }

  auto* dispatcher = StateOf(runtime).GetFunctionDispatcher();
  const u32 caller_module_base = dispatcher->FindCallerModuleBase(caller_address);
  const KernelModuleState::ThunkKey key{caller_module_base, ordinal};

  auto thunk_it = module.kernel.thunk_cache.find(key);
  if (thunk_it != module.kernel.thunk_cache.end()) {
    REXSYS_DEBUG("GetProcAddressByOrdinal: {} ({:04X}) in {} -> cached thunk {:08X}",
                 export_entry->name, ordinal, module.name, thunk_it->second);
    return thunk_it->second;
  }

  const u32 thunk_addr = dispatcher->AllocateThunk(export_entry->fn, caller_address);
  if (thunk_addr) {
    module.kernel.thunk_cache[key] = thunk_addr;
    REXSYS_DEBUG("GetProcAddressByOrdinal: {} ({:04X}) in {} -> thunk at {:08X}",
                 export_entry->name, ordinal, module.name, thunk_addr);
    return thunk_addr;
  }

  REXSYS_WARN("GetProcAddressByOrdinal: function {} ({:04X}) in {} - no thunk available",
              export_entry->name, ordinal, module.name);
  return 0;
}

u32 UserProcAddressByOrdinal(TitleRuntime& runtime, ModuleObject& module, u16 ordinal,
                             u32 caller_address) {
  const auto* xex = module.GetXEX();
  const u32 guest_addr = xex ? xex->GetExportAddress(ordinal) : 0;
  if (!guest_addr || !caller_address) {
    return guest_addr;
  }

  auto* dispatcher = StateOf(runtime).GetFunctionDispatcher();
  auto* func = dispatcher->GetFunction(guest_addr);
  if (!func) {
    return guest_addr;
  }

  const KernelModuleState::ThunkKey key{dispatcher->FindCallerModuleBase(caller_address), ordinal};
  if (auto cached = module.user.thunk_cache.find(key); cached != module.user.thunk_cache.end()) {
    return cached->second;
  }
  const u32 thunk_addr = dispatcher->AllocateThunk(func, caller_address);
  if (thunk_addr) {
    module.user.thunk_cache[key] = thunk_addr;
  }
  return thunk_addr;
}

}  // namespace

ModuleObject::ModuleObject(TitleRuntime* runtime_in, ModuleType type_in)
    : type(type_in), runtime(runtime_in) {
  hmodule_ptr =
      StateOf(*runtime).GetAddressSpace()->SystemHeapAlloc(sizeof(X_LDR_DATA_TABLE_ENTRY));
}

ModuleObject::~ModuleObject() {
  if (type == ModuleType::kUser) {
    Unload();
  }
  StateOf(*runtime).GetAddressSpace()->SystemHeapFree(hmodule_ptr);
}

X_HANDLE CreateModule(TitleRuntime& runtime, ModuleType type) {
  Result<X_HANDLE> added =
      runtime.GetObjectTable().Emplace<ModuleObject>(AddOptions{}, &runtime, type);
  if (!added) {
    return 0;
  }
  const X_HANDLE handle = *added;
  auto module = runtime.GetObjectTable().Get<ModuleObject>(handle);
  auto ldr_data = StateOf(runtime).GetAddressSpace()->TranslateVirtual<X_LDR_DATA_TABLE_ENTRY*>(
      module->hmodule_ptr);
  ldr_data->checksum = handle;
  return handle;
}

bool ModuleObject::IsNamed(std::string_view query) const {
  return rex::string::utf8_equal_case(rex::string::utf8_find_name_from_guest_path(path), query) ||
         rex::string::utf8_equal_case(name, query) || rex::string::utf8_equal_case(path, query);
}

X_STATUS ModuleObject::GetSection(std::string_view section_name, u32* out_section_data,
                                  u32* out_section_size) const {
  if (type == ModuleType::kKernel) {
    return X_STATUS_UNSUCCESSFUL;
  }

  const auto* xex = GetXEX();
  if (!xex) {
    return X_STATUS_NOT_FOUND;
  }

  const auto* resource_header =
      xex->GetImage().GetOptHeader<XEXOptResourceInfo>(kXEXHeaderResourceInfo);
  if (!resource_header) {
    return X_STATUS_NOT_FOUND;
  }
  const u32 count = (resource_header->size - 4) / sizeof(XEXResource);
  for (u32 i = 0; i < count; i++) {
    const auto& res = resource_header->resources[i];
    if (rex::string::utf8_equal_z(section_name, std::string_view(res.name, 8))) {
      *out_section_data = res.address;
      *out_section_size = res.size;
      return X_STATUS_SUCCESS;
    }
  }

  return X_STATUS_NOT_FOUND;
}

u32 ModuleObject::HandleFromHModule(void* hmodule) {
  auto ldr_data = reinterpret_cast<X_LDR_DATA_TABLE_ENTRY*>(hmodule);
  return ldr_data->checksum;
}

X_HANDLE ModuleObject::FromHModule(TitleRuntime& runtime, void* hmodule) {
  const X_HANDLE handle = HandleFromHModule(hmodule);
  if (auto pin = runtime.GetObjectTable().Get(handle);
      pin && pin.GetType() == ObjectType::kModule) {
    return handle;
  }
  return 0;
}

u32 ModuleObject::GetExportAddress(TitleRuntime& owning_runtime, u16 ordinal, u32 caller_address) {
  if (type == ModuleType::kKernel) {
    return KernelProcAddressByOrdinal(owning_runtime, *this, ordinal, caller_address);
  }
  return UserProcAddressByOrdinal(owning_runtime, *this, ordinal, caller_address);
}

u32 ModuleObject::GetExportAddress(std::string_view export_name) const {
  if (type == ModuleType::kKernel) {
    REXSYS_FAIL(0, "GetExportAddress by name is not implemented for kernel modules");
  }
  const auto* xex = GetXEX();
  return xex ? xex->GetExportAddress(export_name) : 0;
}

}  // namespace rex::kernel
