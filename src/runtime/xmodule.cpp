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

#include <rex/logging.h>
#include <rex/runtime/exports.h>
#include <rex/string.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime/kernel_module.h>
#include <rex/runtime.h>
#include <rex/runtime/user_module.h>
#include <rex/runtime/xmodule.h>

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

  if (export_entry->kind == rex::runtime::ExportKind::Variable) {
    const u32 address = KernelModuleVariable(module, ordinal);
    REXLOG_DEBUG(system, "GetProcAddressByOrdinal: {} ({:04X}) -> variable at {:08X}",
                 export_entry->name, ordinal, address);
    return address;
  }

  auto* dispatcher = runtime.function_dispatcher();
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
  const u32 guest_addr = UserModuleXEX(module)->GetProcAddress(ordinal);
  if (!guest_addr || !caller_address) {
    return guest_addr;
  }

  auto* dispatcher = runtime.function_dispatcher();
  auto* func = dispatcher->GetFunction(guest_addr);
  if (!func) {
    return guest_addr;
  }

  return dispatcher->AllocateThunk(func, caller_address);
}

}  // namespace

ModuleObject::ModuleObject(Runtime* runtime_in, ModuleType type_in)
    : type(type_in), runtime(runtime_in) {
  hmodule_ptr = runtime->memory()->SystemHeapAlloc(sizeof(X_LDR_DATA_TABLE_ENTRY));
}

ModuleObject::~ModuleObject() {
  if (type == ModuleType::User) {
    UserModuleUnload(*this);
  }
  runtime->memory()->SystemHeapFree(hmodule_ptr);
}

X_HANDLE CreateModule(Runtime& runtime, ModuleType type) {
  Result<X_HANDLE> added =
      runtime.object_table().Emplace<ModuleObject>(AddOptions{}, &runtime, type);
  if (!added) {
    return 0;
  }
  const X_HANDLE handle = *added;
  auto module = runtime.object_table().Get<ModuleObject>(handle);
  auto ldr_data = runtime.memory()->TranslateVirtual<X_LDR_DATA_TABLE_ENTRY*>(module->hmodule_ptr);
  ldr_data->checksum = handle;
  return handle;
}

bool ModuleMatches(const ModuleObject& module, std::string_view name) {
  return rex::utf8_equal_case(rex::utf8_find_name_from_guest_path(module.path), name) ||
         rex::utf8_equal_case(module.name, name) || rex::utf8_equal_case(module.path, name);
}

X_STATUS ModuleGetSection(const ModuleObject& module, std::string_view name, u32* out_section_data,
                          u32* out_section_size) {
  if (module.type == ModuleType::Kernel) {
    return X_STATUS_UNSUCCESSFUL;
  }

  xex2_opt_resource_info* resource_header = nullptr;
  if (!runtime::XexModule::GetOptHeader(UserModuleXEX(module)->xex_header(),
                                        XEX_HEADER_RESOURCE_INFO, &resource_header)) {
    return X_STATUS_NOT_FOUND;
  }
  const u32 count = (resource_header->size - 4) / sizeof(xex2_resource);
  for (u32 i = 0; i < count; i++) {
    auto& res = resource_header->resources[i];
    if (rex::utf8_equal_z(name, std::string_view(res.name, 8))) {
      *out_section_data = res.address;
      *out_section_size = res.size;
      return X_STATUS_SUCCESS;
    }
  }

  return X_STATUS_NOT_FOUND;
}

u32 ModuleHandleFromHModule(void* hmodule) {
  auto ldr_data = reinterpret_cast<X_LDR_DATA_TABLE_ENTRY*>(hmodule);
  return ldr_data->checksum;
}

X_HANDLE ModuleFromHModule(Runtime& runtime, void* hmodule) {
  const X_HANDLE handle = ModuleHandleFromHModule(hmodule);
  if (auto pin = runtime.object_table().Get(handle); pin && pin.kind() == ObjectKind::Module) {
    return handle;
  }
  return 0;
}

u32 ModuleGetProcAddressByOrdinal(Runtime& runtime, ModuleObject& module, u16 ordinal,
                                  u32 caller_address) {
  if (module.type == ModuleType::Kernel) {
    return KernelProcAddressByOrdinal(runtime, module, ordinal, caller_address);
  }
  return UserProcAddressByOrdinal(runtime, module, ordinal, caller_address);
}

u32 ModuleGetProcAddressByName(ModuleObject& module, std::string_view name) {
  if (module.type == ModuleType::Kernel) {
    REX_FAIL(system, 0, "ModuleGetProcAddressByName is not implemented for kernel modules");
  }
  return UserModuleXEX(module)->GetProcAddress(name);
}

}  // namespace rex::runtime
