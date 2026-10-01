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
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/string.h>
#include <rex/system/elf_module.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xex_module.h>
#include <rex/system/xfile.h>

namespace rex::system {

namespace {

X_STATUS LoadXexContinue(KernelState* kernel_state, ModuleObject& module) {
  if (!UserModuleXex(module)) {
    return X_STATUS_UNSUCCESSFUL;
  }

  if (module.user.guest_xex_header) {
    return X_STATUS_SUCCESS;
  }

  if (!UserModuleXex(module)->LoadContinue()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  auto* memory = kernel_state->memory();
  auto header = UserModuleXex(module)->xex_header();
  auto security_header = UserModuleXex(module)->xex_security_info();
  module.user.guest_xex_header = memory->SystemHeapAlloc(header->header_size);

  u8* xex_header_ptr = memory->TranslateVirtual(module.user.guest_xex_header);
  std::memcpy(xex_header_ptr, header, header->header_size);

  UserModuleXex(module)->GetOptHeader(XEX_HEADER_ENTRY_POINT, &module.user.entry_point);
  UserModuleXex(module)->GetOptHeader(XEX_HEADER_DEFAULT_STACK_SIZE, &module.user.stack_size);
  module.user.is_dll_module = !!(header->module_flags & XEX_MODULE_DLL_MODULE);

  auto ldr_data = memory->TranslateVirtual<X_LDR_DATA_TABLE_ENTRY*>(module.hmodule_ptr);
  ldr_data->dll_base = 0;
  ldr_data->xex_header_base = module.user.guest_xex_header;
  ldr_data->full_image_size = security_header->image_size;
  ldr_data->image_base = UserModuleXex(module)->base_address();
  ldr_data->entry_point = module.user.entry_point;

  return X_STATUS_SUCCESS;
}

}  // namespace

X_HANDLE CreateUserModule(KernelState* kernel_state) {
  return CreateModule(kernel_state, ModuleType::User);
}

rex::runtime::XexModule* UserModuleXex(const ModuleObject& module) {
  assert_true(module.user.module_format == ModuleFormat::Xex);
  return static_cast<rex::runtime::XexModule*>(module.processor_module);
}

u32 UserModuleTitleID(const ModuleObject& module) {
  if (module.user.module_format != ModuleFormat::Xex) {
    return 0;
  }
  auto header = UserModuleXex(module)->xex_header();
  for (u32 i = 0; i < header->header_count; i++) {
    auto& opt_header = header->headers[i];
    if (opt_header.key == XEX_HEADER_EXECUTION_INFO) {
      auto opt_header_ptr = reinterpret_cast<const u8*>(header) + opt_header.offset;
      auto opt_exec_info = reinterpret_cast<const xex2_opt_execution_info*>(opt_header_ptr);
      return static_cast<u32>(opt_exec_info->title_id);
    }
  }
  return 0;
}

X_STATUS UserModuleLoadFromFile(KernelState* kernel_state, ModuleObject& module,
                                std::string_view path) {
  X_STATUS result = X_STATUS_UNSUCCESSFUL;

  auto* fs = kernel_state->file_system();
  auto fs_entry = fs->Stat(path);
  if (!fs_entry) {
    REX_FAIL(system, X_STATUS_NO_SUCH_FILE, "File not found: {}", path);
  }

  module.path = fs_entry->absolute_path;
  module.name = rex::utf8_find_base_name_from_guest_path(module.path);

  filesystem::OpenOptions options;
  options.access = filesystem::Access::kGenericRead;
  auto opened = fs->Open(module.path, options);
  if (!opened) {
    return ToXStatus(opened.error());
  }
  if (auto view = opened->file->View()) {
    result = UserModuleLoadFromMemory(kernel_state, module, view->data(), view->size());
  } else {
    std::vector<u8> buffer(fs_entry->size);
    auto bytes_read = opened->file->Read(buffer, 0);
    if (!bytes_read) {
      return ToXStatus(bytes_read.error());
    }
    result = UserModuleLoadFromMemory(kernel_state, module, buffer.data(), *bytes_read);
  }

  if (result != X_STATUS_PENDING) {
    return result;
  }

  if (auto patch_entry = fs->Stat(module.path + "p")) {
    auto patch_path = patch_entry->absolute_path;

    REXLOG_DEBUG(system, "Loading XEX patch from {}", patch_path);

    ModuleObject patch_module(kernel_state, ModuleType::User);
    result = UserModuleLoadFromFile(kernel_state, patch_module, patch_path);
    if (!result) {
      result = UserModuleXex(patch_module)->ApplyPatch(UserModuleXex(module));
      if (result) {
        REXLOG_ERROR(system, "Failed to apply XEX patch, code: {}", result);
      }
    } else {
      REXLOG_ERROR(system, "Failed to load XEX patch, code: {}", result);
    }

    if (result) {
      return X_STATUS_UNSUCCESSFUL;
    }
  }

  return LoadXexContinue(kernel_state, module);
}

X_STATUS UserModuleLoadFromMemory(KernelState* kernel_state, ModuleObject& module, const void* addr,
                                  size_t length) {
  auto* dispatcher = kernel_state->function_dispatcher();

  be<memory::fourcc_t> magic;
  magic.value = memory::load<memory::fourcc_t>(addr);

  if (magic == runtime::kXEX2Signature || magic == runtime::kXEX1Signature) {
    module.user.module_format = ModuleFormat::Xex;
  } else if (magic == runtime::kElfSignature) {
    module.user.module_format = ModuleFormat::Elf;
  } else {
    REX_FAIL(system, X_STATUS_NOT_IMPLEMENTED, "Unknown module magic: {:08X}", u32(magic));
  }

  if (module.user.module_format == ModuleFormat::Xex) {
    auto xex_module = new runtime::XexModule(dispatcher, kernel_state);
    if (!xex_module->Load(module.name, module.path, addr, length)) {
      delete xex_module;
      return X_STATUS_UNSUCCESSFUL;
    }

    module.processor_module = xex_module;
    return X_STATUS_PENDING;
  }

  if (module.user.module_format == ModuleFormat::Elf) {
    auto elf_module = new runtime::ElfModule(dispatcher, kernel_state);
    if (!elf_module->Load(module.name, module.path, addr, length)) {
      delete elf_module;
      return X_STATUS_UNSUCCESSFUL;
    }

    module.user.entry_point = elf_module->entry_point();
    module.user.stack_size = 1024 * 1024;
    module.user.is_dll_module = false;

    module.processor_module = elf_module;
    return X_STATUS_SUCCESS;
  }

  return X_STATUS_UNSUCCESSFUL;
}

X_STATUS UserModuleUnload(ModuleObject& module) {
  if (module.user.module_format == ModuleFormat::Xex &&
      (!module.processor_module || !UserModuleXex(module)->loaded())) {
    return X_STATUS_SUCCESS;
  }

  if (module.user.module_format == ModuleFormat::Xex && module.processor_module &&
      UserModuleXex(module)->Unload()) {
    return X_STATUS_SUCCESS;
  }

  return X_STATUS_UNSUCCESSFUL;
}

X_STATUS UserModuleGetOptHeader(ModuleObject& module, xex2_header_keys key, void** out_ptr) {
  assert_not_null(out_ptr);

  if (module.user.module_format == ModuleFormat::Elf) {
    return X_STATUS_UNSUCCESSFUL;
  }

  if (!UserModuleXex(module)->GetOptHeader(key, out_ptr)) {
    return X_STATUS_NOT_FOUND;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS UserModuleGetOptHeader(ModuleObject& module, xex2_header_keys key,
                                u32* out_header_guest_ptr) {
  if (module.user.module_format == ModuleFormat::Elf) {
    return X_STATUS_UNSUCCESSFUL;
  }

  auto* memory = module.kernel_state->memory();
  auto header = memory->TranslateVirtual<const xex2_header*>(module.user.guest_xex_header);
  if (!header) {
    return X_STATUS_UNSUCCESSFUL;
  }
  return UserModuleGetOptHeader(memory, header, key, out_header_guest_ptr);
}

X_STATUS UserModuleGetOptHeader(const memory::Memory* memory, const xex2_header* header,
                                xex2_header_keys key, u32* out_header_guest_ptr) {
  assert_not_null(out_header_guest_ptr);
  u32 field_value = 0;
  bool field_found = false;
  for (u32 i = 0; i < header->header_count; i++) {
    auto& opt_header = header->headers[i];
    if (opt_header.key != key) {
      continue;
    }
    field_found = true;
    switch (opt_header.key & 0xFF) {
      case 0x00:
        field_value = opt_header.value;
        break;
      case 0x01:
        field_value = memory->HostToGuestVirtual(&opt_header.value);
        break;
      default:
        field_value = memory->HostToGuestVirtual(header) + opt_header.offset;
        break;
    }
    break;
  }
  *out_header_guest_ptr = field_value;
  if (!field_found) {
    return X_STATUS_NOT_FOUND;
  }
  return X_STATUS_SUCCESS;
}

}  // namespace rex::system
