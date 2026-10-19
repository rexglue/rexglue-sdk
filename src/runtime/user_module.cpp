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
#include <rex/fail.h>
#include <rex/logging/log.h>
#include <rex/string.h>
#include <rex/runtime/elf_module.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime.h>
#include <rex/runtime/module_object.h>
#include <rex/runtime/xex_module.h>
#include <rex/runtime/file_object.h>

namespace rex::runtime {

namespace {

X_STATUS LoadXEXContinue(Runtime& runtime, ModuleObject& module) {
  if (!module.GetXEX()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  if (module.user.guest_xex_header) {
    return X_STATUS_SUCCESS;
  }

  if (!module.GetXEX()->LoadContinue()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  auto* memory = runtime.GetMemory();
  const auto& image = module.GetXEX()->GetImage();
  const auto header = image.GetHeaderBytes();
  module.user.guest_xex_header = memory->SystemHeapAlloc(static_cast<u32>(header.size()));

  u8* xex_header_ptr = memory->TranslateVirtual(module.user.guest_xex_header);
  std::memcpy(xex_header_ptr, header.data(), header.size());

  module.user.entry_point =
      image.OptHeaderValue(filesystem::kXEXHeaderEntryPoint).value_or(module.user.entry_point);
  module.user.stack_size =
      image.OptHeaderValue(filesystem::kXEXHeaderDefaultStackSize).value_or(module.user.stack_size);
  module.user.is_dll_module = !!(image.GetHeader().module_flags & filesystem::kXEXModuleDLLModule);

  auto ldr_data = memory->TranslateVirtual<X_LDR_DATA_TABLE_ENTRY*>(module.hmodule_ptr);
  ldr_data->dll_base = 0;
  ldr_data->xex_header_base = module.user.guest_xex_header;
  ldr_data->full_image_size = image.GetSecurity().image_size;
  ldr_data->image_base = module.GetXEX()->GetBaseAddress();
  ldr_data->entry_point = module.user.entry_point;
  ldr_data->load_count = 1;

  return X_STATUS_SUCCESS;
}

}  // namespace

X_HANDLE ModuleObject::CreateUserModule(Runtime& runtime) {
  return CreateModule(runtime, ModuleType::kUser);
}

rex::runtime::XEXModule* ModuleObject::GetXEX() {
  if (user.module_format != ModuleFormat::kXEX) {
    return nullptr;
  }
  return static_cast<rex::runtime::XEXModule*>(processor_module.get());
}

const rex::runtime::XEXModule* ModuleObject::GetXEX() const {
  if (user.module_format != ModuleFormat::kXEX) {
    return nullptr;
  }
  return static_cast<const rex::runtime::XEXModule*>(processor_module.get());
}

u32 ModuleObject::GetTitleId() const {
  if (user.module_format != ModuleFormat::kXEX) {
    return 0;
  }
  const auto* info = GetXEX()->GetImage().OptHeader<filesystem::XEXOptExecutionInfo>(
      filesystem::kXEXHeaderExecutionInfo);
  return info ? static_cast<u32>(info->title_id) : 0;
}

X_STATUS ModuleObject::LoadFromFile(Runtime& owning_runtime, std::string_view guest_path) {
  X_STATUS result = X_STATUS_UNSUCCESSFUL;

  auto* fs = owning_runtime.GetFileSystem();
  auto fs_entry = fs->Stat(guest_path);
  if (!fs_entry) {
    REX_FAIL(system, X_STATUS_NO_SUCH_FILE, "File {} was not found", guest_path);
  }

  path = fs_entry->absolute_path;
  name = rex::utf8_find_base_name_from_guest_path(path);

  filesystem::OpenOptions options;
  options.access = filesystem::Access::kGenericRead;
  auto opened = fs->Open(path, options);
  if (!opened) {
    return ToXStatus(opened.error());
  }
  if (auto view = opened->file->View()) {
    result = LoadFromMemory(owning_runtime, view->data(), view->size());
  } else {
    std::vector<u8> buffer(fs_entry->size);
    auto bytes_read = opened->file->Read(buffer, 0);
    if (!bytes_read) {
      return ToXStatus(bytes_read.error());
    }
    result = LoadFromMemory(owning_runtime, buffer.data(), *bytes_read);
  }

  if (result != X_STATUS_PENDING) {
    return result;
  }

  if (auto patch_entry = fs->Stat(path + "p")) {
    auto patch_path = patch_entry->absolute_path;

    REXLOG_DEBUG(system, "Loading XEX patch from {}", patch_path);

    ModuleObject patch_module(&owning_runtime, ModuleType::kUser);
    result = patch_module.LoadFromFile(owning_runtime, patch_path);
    if (!result) {
      result = patch_module.GetXEX() && GetXEX() ? patch_module.GetXEX()->ApplyPatch(GetXEX())
                                                 : X_STATUS_UNSUCCESSFUL;
      if (result) {
        REXLOG_ERROR(system, "Failed to apply XEX patch, status {}", result);
      }
    } else {
      REXLOG_ERROR(system, "Failed to load XEX patch, status {}", result);
    }

    if (result) {
      return X_STATUS_UNSUCCESSFUL;
    }
  }

  return LoadXEXContinue(owning_runtime, *this);
}

X_STATUS ModuleObject::LoadFromMemory(Runtime& owning_runtime, const void* addr, size_t length) {
  auto* dispatcher = owning_runtime.GetFunctionDispatcher();

  BE<memory::FourCCT> magic;
  magic.value = memory::load<memory::FourCCT>(addr);

  if (magic == runtime::kXEX2Signature || magic == runtime::kXEX1Signature) {
    user.module_format = ModuleFormat::kXEX;
  } else if (magic == runtime::kELFSignature) {
    user.module_format = ModuleFormat::kELF;
  } else {
    REX_FAIL(system, X_STATUS_NOT_IMPLEMENTED, "Unknown module magic {:08X}",
             static_cast<u32>(magic));
  }

  if (user.module_format == ModuleFormat::kXEX) {
    auto xex_module = std::make_unique<runtime::XEXModule>(dispatcher, owning_runtime);
    if (!xex_module->Load(name, path, addr, length)) {
      return X_STATUS_UNSUCCESSFUL;
    }

    processor_module = std::move(xex_module);
    return X_STATUS_PENDING;
  }

  if (user.module_format == ModuleFormat::kELF) {
    auto elf_module = std::make_unique<runtime::ELFModule>(dispatcher);
    if (!elf_module->Load(name, path, addr, length)) {
      return X_STATUS_UNSUCCESSFUL;
    }

    user.entry_point = elf_module->GetEntryPoint();
    user.stack_size = 1024 * 1024;
    user.is_dll_module = false;

    processor_module = std::move(elf_module);
    return X_STATUS_SUCCESS;
  }

  return X_STATUS_UNSUCCESSFUL;
}

X_STATUS ModuleObject::Unload() {
  if (user.module_format == ModuleFormat::kXEX && (!processor_module || !GetXEX()->GetLoaded())) {
    return X_STATUS_SUCCESS;
  }

  if (user.module_format == ModuleFormat::kXEX && processor_module && GetXEX()->Unload()) {
    return X_STATUS_SUCCESS;
  }

  return X_STATUS_UNSUCCESSFUL;
}

X_STATUS ModuleObject::OptHeader(filesystem::XEXHeaderKey key, void** out_ptr) {
  REX_ASSERT(out_ptr != nullptr);

  if (!GetXEX()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  const auto& image = GetXEX()->GetImage();
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
  if (user.module_format == ModuleFormat::kELF) {
    return X_STATUS_UNSUCCESSFUL;
  }

  auto* memory = runtime->GetMemory();
  auto header = memory->TranslateVirtual<const filesystem::XEXHeader*>(user.guest_xex_header);
  if (!header) {
    return X_STATUS_UNSUCCESSFUL;
  }
  return OptHeader(memory, header, key, out_header_guest_ptr);
}

X_STATUS ModuleObject::OptHeader(const runtime::Memory* memory, const filesystem::XEXHeader* header,
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
