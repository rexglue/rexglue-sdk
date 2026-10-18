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

#include <rex/fail.h>
#include <rex/logging/log.h>
#include <rex/runtime/exports.h>
#include <rex/runtime/object_table.h>
#include <rex/string.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime.h>
#include <rex/runtime/module_object.h>

namespace rex::runtime {

namespace {

u32 KernelProcAddressByOrdinal(Runtime& runtime, ModuleObject& module, u16 ordinal,
                               u32 caller_address) {
  const auto runtime_module = rex::runtime::ModuleFromName(module.name);
  const auto* export_entry =
      runtime_module ? rex::runtime::FindByOrdinal(*runtime_module, ordinal) : nullptr;
  if (!export_entry) {
    REXLOG_DEBUG(system, "GetProcAddressByOrdinal: ordinal {:04X} not found in {}", ordinal,
                 module.name);
    return 0;
  }

  if (export_entry->kind == rex::runtime::ExportKind::kVariable) {
    const u32 address = module.Variable(ordinal);
    REXLOG_DEBUG(system, "GetProcAddressByOrdinal: {} ({:04X}) -> variable at {:08X}",
                 export_entry->name, ordinal, address);
    return address;
  }

  auto* dispatcher = runtime.GetFunctionDispatcher();
  const u32 caller_module_base = dispatcher->FindCallerModuleBase(caller_address);
  const KernelModuleData::ThunkKey key{caller_module_base, ordinal};

  auto thunk_it = module.kernel.thunk_cache.find(key);
  if (thunk_it != module.kernel.thunk_cache.end()) {
    REXLOG_DEBUG(system, "GetProcAddressByOrdinal: {} ({:04X}) in {} -> cached thunk {:08X}",
                 export_entry->name, ordinal, module.name, thunk_it->second);
    return thunk_it->second;
  }

  const u32 thunk_addr = dispatcher->AllocateThunk(export_entry->fn, caller_address);
  if (thunk_addr) {
    module.kernel.thunk_cache[key] = thunk_addr;
    REXLOG_DEBUG(system, "GetProcAddressByOrdinal: {} ({:04X}) in {} -> thunk at {:08X}",
                 export_entry->name, ordinal, module.name, thunk_addr);
    return thunk_addr;
  }

  REXLOG_WARN(system, "GetProcAddressByOrdinal: function {} ({:04X}) in {} - no thunk available",
              export_entry->name, ordinal, module.name);
  return 0;
}

u32 UserProcAddressByOrdinal(Runtime& runtime, ModuleObject& module, u16 ordinal,
                             u32 caller_address) {
  const auto* xex = module.GetXEX();
  const u32 guest_addr = xex ? xex->GetProcAddress(ordinal) : 0;
  if (!guest_addr || !caller_address) {
    return guest_addr;
  }

  auto* dispatcher = runtime.GetFunctionDispatcher();
  auto* func = dispatcher->GetFunction(guest_addr);
  if (!func) {
    return guest_addr;
  }

  const KernelModuleData::ThunkKey key{dispatcher->FindCallerModuleBase(caller_address), ordinal};
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

ModuleObject::ModuleObject(Runtime* runtime_in, ModuleType type_in)
    : type(type_in), runtime(runtime_in) {
  hmodule_ptr = runtime->GetMemory()->SystemHeapAlloc(sizeof(X_LDR_DATA_TABLE_ENTRY));
}

ModuleObject::~ModuleObject() {
  if (type == ModuleType::kUser) {
    Unload();
  }
  runtime->GetMemory()->SystemHeapFree(hmodule_ptr);
}

X_HANDLE CreateModule(Runtime& runtime, ModuleType type) {
  Result<X_HANDLE> added =
      runtime.GetObjectTable().Emplace<ModuleObject>(AddOptions{}, &runtime, type);
  if (!added) {
    return 0;
  }
  const X_HANDLE handle = *added;
  auto module = runtime.GetObjectTable().Get<ModuleObject>(handle);
  auto ldr_data =
      runtime.GetMemory()->TranslateVirtual<X_LDR_DATA_TABLE_ENTRY*>(module->hmodule_ptr);
  ldr_data->checksum = handle;
  return handle;
}

bool ModuleObject::Matches(std::string_view query) const {
  return rex::utf8_equal_case(rex::utf8_find_name_from_guest_path(path), query) ||
         rex::utf8_equal_case(name, query) || rex::utf8_equal_case(path, query);
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
      xex->GetImage().OptHeader<filesystem::XEXOptResourceInfo>(filesystem::kXEXHeaderResourceInfo);
  if (!resource_header) {
    return X_STATUS_NOT_FOUND;
  }
  const u32 count = (resource_header->size - 4) / sizeof(filesystem::XEXResource);
  for (u32 i = 0; i < count; i++) {
    const auto& res = resource_header->resources[i];
    if (rex::utf8_equal_z(section_name, std::string_view(res.name, 8))) {
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

X_HANDLE ModuleObject::FromHModule(Runtime& runtime, void* hmodule) {
  const X_HANDLE handle = HandleFromHModule(hmodule);
  if (auto pin = runtime.GetObjectTable().Get(handle);
      pin && pin.GetKind() == ObjectKind::kModule) {
    return handle;
  }
  return 0;
}

u32 ModuleObject::GetProcAddress(Runtime& owning_runtime, u16 ordinal, u32 caller_address) {
  if (type == ModuleType::kKernel) {
    return KernelProcAddressByOrdinal(owning_runtime, *this, ordinal, caller_address);
  }
  return UserProcAddressByOrdinal(owning_runtime, *this, ordinal, caller_address);
}

u32 ModuleObject::GetProcAddress(std::string_view export_name) const {
  if (type == ModuleType::kKernel) {
    REX_FAIL(system, 0, "GetProcAddress by name is not implemented for kernel modules");
  }
  const auto* xex = GetXEX();
  return xex ? xex->GetProcAddress(export_name) : 0;
}

}  // namespace rex::runtime
