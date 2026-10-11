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
#include <rex/filesystem/xex.h>
#include <rex/logging.h>
#include <rex/string.h>
#include <rex/runtime/elf_module.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime.h>
#include <rex/runtime/user_module.h>
#include <rex/runtime/xex_module.h>
#include <rex/runtime/file_object.h>

namespace rex::runtime {

namespace {

X_STATUS LoadXEXContinue(Runtime& runtime, ModuleObject& module) {
  if (!module.xex()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  if (module.user.guest_xex_header) {
    return X_STATUS_SUCCESS;
  }

  if (!UserModuleXEX(module)->LoadContinue()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  auto* memory = runtime.memory();
  const auto& image = UserModuleXEX(module)->image();
  const auto header = image.header_bytes();
  module.user.guest_xex_header = memory->SystemHeapAlloc(static_cast<u32>(header.size()));

  u8* xex_header_ptr = memory->TranslateVirtual(module.user.guest_xex_header);
  std::memcpy(xex_header_ptr, header.data(), header.size());

  module.user.entry_point =
      image.OptHeaderValue(filesystem::XEX_HEADER_ENTRY_POINT).value_or(module.user.entry_point);
  module.user.stack_size = image.OptHeaderValue(filesystem::XEX_HEADER_DEFAULT_STACK_SIZE)
                               .value_or(module.user.stack_size);
  module.user.is_dll_module = !!(image.header().module_flags & filesystem::XEX_MODULE_DLL_MODULE);

  auto ldr_data = memory->TranslateVirtual<X_LDR_DATA_TABLE_ENTRY*>(module.hmodule_ptr);
  ldr_data->dll_base = 0;
  ldr_data->xex_header_base = module.user.guest_xex_header;
  ldr_data->full_image_size = image.security().image_size;
  ldr_data->image_base = UserModuleXEX(module)->base_address();
  ldr_data->entry_point = module.user.entry_point;

  return X_STATUS_SUCCESS;
}

}  // namespace

X_HANDLE CreateUserModule(Runtime& runtime) {
  return CreateModule(runtime, ModuleType::User);
}

rex::runtime::XEXModule* ModuleObject::xex() {
  REX_ASSERT(user.module_format == ModuleFormat::XEX);
  return static_cast<rex::runtime::XEXModule*>(processor_module.get());
}

const rex::runtime::XEXModule* ModuleObject::xex() const {
  REX_ASSERT(user.module_format == ModuleFormat::XEX);
  return static_cast<const rex::runtime::XEXModule*>(processor_module.get());
}

u32 ModuleObject::title_id() const {
  if (user.module_format != ModuleFormat::XEX) {
    return 0;
  }
  const auto* info = UserModuleXEX(module)->image().OptHeader<filesystem::XEXOptExecutionInfo>(
      filesystem::XEX_HEADER_EXECUTION_INFO);
  return info ? static_cast<u32>(info->title_id) : 0;
}

X_STATUS UserModuleLoadFromFile(Runtime& runtime, ModuleObject& module, std::string_view path) {
  X_STATUS result = X_STATUS_UNSUCCESSFUL;

  auto* fs = runtime.file_system();
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
    result = UserModuleLoadFromMemory(runtime, module, view->data(), view->size());
  } else {
    std::vector<u8> buffer(fs_entry->size);
    auto bytes_read = opened->file->Read(buffer, 0);
    if (!bytes_read) {
      return ToXStatus(bytes_read.error());
    }
    result = UserModuleLoadFromMemory(runtime, module, buffer.data(), *bytes_read);
  }

  if (result != X_STATUS_PENDING) {
    return result;
  }

  if (auto patch_entry = fs->Stat(module.path + "p")) {
    auto patch_path = patch_entry->absolute_path;

    REXLOG_DEBUG(system, "Loading XEX patch from {}", patch_path);

    ModuleObject patch_module(&runtime, ModuleType::User);
    result = UserModuleLoadFromFile(runtime, patch_module, patch_path);
    if (!result) {
      result = UserModuleXEX(patch_module)->ApplyPatch(UserModuleXEX(module));
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

  return LoadXEXContinue(runtime, *this);
}

X_STATUS UserModuleLoadFromMemory(Runtime& runtime, ModuleObject& module, const void* addr,
                                  size_t length) {
  auto* dispatcher = runtime.function_dispatcher();

  be<memory::fourcc_t> magic;
  magic.value = memory::load<memory::fourcc_t>(addr);

  if (magic == runtime::kXEX2Signature || magic == runtime::kXEX1Signature) {
    module.user.module_format = ModuleFormat::Xex;
  } else if (magic == runtime::kElfSignature) {
    module.user.module_format = ModuleFormat::Elf;
  } else {
    REX_FAIL(system, X_STATUS_NOT_IMPLEMENTED, "Unknown module magic: {:08X}",
             static_cast<u32>(magic));
  }

  if (module.user.module_format == ModuleFormat::Xex) {
    auto xex_module = std::make_unique<runtime::XexModule>(dispatcher, runtime);
    if (!xex_module->Load(module.name, module.path, addr, length)) {
      return X_STATUS_UNSUCCESSFUL;
    }

    module.processor_module = xex_module.release();
    return X_STATUS_PENDING;
  }

  if (module.user.module_format == ModuleFormat::Elf) {
    auto elf_module = std::make_unique<runtime::ElfModule>(dispatcher);
    if (!elf_module->Load(module.name, module.path, addr, length)) {
      return X_STATUS_UNSUCCESSFUL;
    }

    module.user.entry_point = elf_module->entry_point();
    module.user.stack_size = 1024 * 1024;
    module.user.is_dll_module = false;

    module.processor_module = elf_module.release();
    return X_STATUS_SUCCESS;
  }

  return X_STATUS_UNSUCCESSFUL;
}

X_STATUS UserModuleUnload(ModuleObject& module) {
  if (module.user.module_format == ModuleFormat::Xex &&
      (!module.processor_module || !UserModuleXEX(module)->loaded())) {
    return X_STATUS_SUCCESS;
  }

  if (module.user.module_format == ModuleFormat::Xex && module.processor_module &&
      UserModuleXEX(module)->Unload()) {
    return X_STATUS_SUCCESS;
  }

  return X_STATUS_UNSUCCESSFUL;
}

X_STATUS UserModuleGetOptHeader(ModuleObject& module, filesystem::XEXHeaderKey key,
                                void** out_ptr) {
  REX_ASSERT(out_ptr != nullptr);

  if (module.user.module_format == ModuleFormat::Elf) {
    return X_STATUS_UNSUCCESSFUL;
  }

  const auto& image = UserModuleXEX(module)->image();
  if ((key & 0xFF) == 0x00) {
    const auto value = image.OptHeaderValue(key);
    if (!value) {
      return X_STATUS_NOT_FOUND;
    }
    *reinterpret_cast<u32*>(out_ptr) = *value;
    return X_STATUS_SUCCESS;
  }
  const void* data = image.OptHeader<void>(key);
  if (!data) {
    return X_STATUS_NOT_FOUND;
  }
  *out_ptr = const_cast<void*>(data);
  return X_STATUS_SUCCESS;
}

X_STATUS ModuleObject::OptHeader(filesystem::XEXHeaderKey key, u32* out_header_guest_ptr) const {
  if (user.module_format == ModuleFormat::Elf) {
    return X_STATUS_UNSUCCESSFUL;
  }

  auto* memory = module.runtime->memory();
  auto header =
      memory->TranslateVirtual<const filesystem::XEXHeader*>(module.user.guest_xex_header);
  if (!header) {
    return X_STATUS_UNSUCCESSFUL;
  }
  return UserModuleGetOptHeader(memory, header, key, out_header_guest_ptr);
}

X_STATUS UserModuleGetOptHeader(const runtime::Memory* memory, const filesystem::XEXHeader* header,
                                filesystem::XEXHeaderKey key, u32* out_header_guest_ptr) {
  REX_ASSERT(out_header_guest_ptr != nullptr);
  const auto entry = filesystem::FindOptHeaderEntry(
      std::span(reinterpret_cast<const u8*>(header), header->header_size), key);
  if (!entry) {
    *out_header_guest_ptr = 0;
    return X_STATUS_NOT_FOUND;
  }
  switch (key & 0xFF) {
    case 0x00:
      *out_header_guest_ptr = entry->value;
      break;
    case 0x01:
      *out_header_guest_ptr = memory->HostToGuestVirtual(&header->headers[entry->index].value);
      break;
    default:
      *out_header_guest_ptr = memory->HostToGuestVirtual(header) + entry->value;
      break;
  }
  return X_STATUS_SUCCESS;
}

}  // namespace rex::runtime
