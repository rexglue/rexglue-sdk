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
#include <string_view>
#include <vector>

#include <rex/filesystem/file_open.h>
#include <rex/filesystem/xex.h>
#include <rex/kernel/types/xex.h>
#include <rex/logging/log.h>
#include <rex/filesystem/path.h>
#include <rex/kernel/module.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/object/module_object.h>
#include <rex/kernel/object/file_object.h>
#include <rex/filesystem/file_system_factory.h>
#include "kernel/rexkernel_module.h"
#include "kernel/title_state.h"

namespace rex::kernel {

namespace {

X_STATUS LoadXEXContinue(TitleRuntime& runtime, ModuleObject& module) {
  if (!module.GetXEX()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  if (module.user.guest_xex_header) {
    return X_STATUS_SUCCESS;
  }

  if (!module.GetXEX()->LoadContinue()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  auto* memory = StateOf(runtime).GetMemory();
  const auto& image = module.GetXEX()->GetImage();
  const auto header = image.GetHeaderBytes();
  module.user.guest_xex_header = memory->SystemHeapAlloc(static_cast<u32>(header.size()));

  u8* xex_header_ptr = memory->TranslateVirtual(module.user.guest_xex_header);
  std::memcpy(xex_header_ptr, header.data(), header.size());

  module.user.entry_point =
      image.GetOptHeaderValue(kXEXHeaderEntryPoint).value_or(module.user.entry_point);
  module.user.stack_size =
      image.GetOptHeaderValue(kXEXHeaderDefaultStackSize).value_or(module.user.stack_size);
  module.user.is_dll_module = !!(image.GetHeader().module_flags & kXEXModuleDLLModule);

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

X_HANDLE ModuleObject::CreateUserModule(TitleRuntime& runtime) {
  return CreateModule(runtime, ModuleType::kUser);
}

rex::kernel::XEXModule* ModuleObject::GetXEX() {
  if (user.module_format != ModuleFormat::kXEX) {
    return nullptr;
  }
  return static_cast<rex::kernel::XEXModule*>(processor_module.get());
}

const rex::kernel::XEXModule* ModuleObject::GetXEX() const {
  if (user.module_format != ModuleFormat::kXEX) {
    return nullptr;
  }
  return static_cast<const rex::kernel::XEXModule*>(processor_module.get());
}

std::vector<u32> ModuleObject::GetAlternateTitleIDs() const {
  if (user.module_format != ModuleFormat::kXEX) {
    return {};
  }
  return GetXEX()->GetImage().GetAlternateTitleIDs();
}

u32 ModuleObject::GetTitleID() const {
  if (user.module_format != ModuleFormat::kXEX) {
    return 0;
  }
  const auto* info =
      GetXEX()->GetImage().GetOptHeader<XEXOptExecutionInfo>(kXEXHeaderExecutionInfo);
  return info ? static_cast<u32>(info->title_id) : 0;
}

X_STATUS ModuleObject::LoadFromFile(TitleRuntime& owning_runtime, std::string_view guest_path) {
  X_STATUS result = X_STATUS_UNSUCCESSFUL;

  auto* fs = StateOf(owning_runtime).GetFileSystem();
  auto fs_entry = fs->Stat(guest_path);
  if (!fs_entry) {
    REXSYS_FAIL(X_STATUS_NO_SUCH_FILE, "File {} was not found", guest_path);
  }

  path = fs_entry->absolute_path;
  name = rex::filesystem::utf8_find_base_name_from_guest_path(path);

  filesystem::OpenOptions options;
  options.access = filesystem::Access::kReadAccess;
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

    REXSYS_DEBUG("Loading XEX patch from {}", patch_path);

    ModuleObject patch_module(&owning_runtime, ModuleType::kUser);
    result = patch_module.LoadFromFile(owning_runtime, patch_path);
    if (!result) {
      result = patch_module.GetXEX() && GetXEX() ? patch_module.GetXEX()->ApplyPatch(GetXEX())
                                                 : X_STATUS_UNSUCCESSFUL;
      if (result) {
        REXSYS_ERROR("Failed to apply XEX patch, status {}", result);
      }
    } else {
      REXSYS_ERROR("Failed to load XEX patch, status {}", result);
    }

    if (result) {
      return X_STATUS_UNSUCCESSFUL;
    }
  }

  return LoadXEXContinue(owning_runtime, *this);
}

X_STATUS ModuleObject::LoadFromMemory(TitleRuntime& owning_runtime, const void* addr,
                                      size_t length) {
  auto* dispatcher = StateOf(owning_runtime).GetFunctionDispatcher();

  BE<memory::FourCCT> magic;
  magic.value = memory::load<memory::FourCCT>(addr);

  if (magic == kernel::kXEX2Signature || magic == kernel::kXEX1Signature) {
    user.module_format = ModuleFormat::kXEX;
  } else if (magic == kernel::kELFSignature) {
    user.module_format = ModuleFormat::kELF;
  } else {
    REXSYS_FAIL(X_STATUS_NOT_IMPLEMENTED, "Unknown module magic {:08X}", static_cast<u32>(magic));
  }

  if (user.module_format == ModuleFormat::kXEX) {
    auto xex_module = std::make_unique<kernel::XEXModule>(dispatcher, owning_runtime);
    if (!xex_module->Load(name, path, addr, length)) {
      return X_STATUS_UNSUCCESSFUL;
    }

    processor_module = std::move(xex_module);
    return X_STATUS_PENDING;
  }

  if (user.module_format == ModuleFormat::kELF) {
    auto elf_module = std::make_unique<kernel::ELFModule>(dispatcher);
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
  if (user.module_format == ModuleFormat::kXEX && (!processor_module || !GetXEX()->IsLoaded())) {
    return X_STATUS_SUCCESS;
  }

  if (user.module_format == ModuleFormat::kXEX && processor_module && GetXEX()->Unload()) {
    return X_STATUS_SUCCESS;
  }

  return X_STATUS_UNSUCCESSFUL;
}

X_STATUS ModuleObject::GetOptHeader(XEXHeaderKey key, void** out_ptr) {
  REX_ASSERT(out_ptr != nullptr);

  if (!GetXEX()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  const auto& image = GetXEX()->GetImage();
  if ((key & 0xFF) == 0x00) {
    const auto value = image.GetOptHeaderValue(key);
    if (!value) {
      return X_STATUS_NOT_FOUND;
    }
    *reinterpret_cast<u32*>(out_ptr) = *value;
    return X_STATUS_SUCCESS;
  }
  const void* data = image.GetOptHeader<void>(key);
  if (!data) {
    return X_STATUS_NOT_FOUND;
  }
  *out_ptr = const_cast<void*>(data);
  return X_STATUS_SUCCESS;
}

X_STATUS ModuleObject::GetOptHeader(XEXHeaderKey key, u32* out_header_guest_ptr) const {
  if (user.module_format == ModuleFormat::kELF) {
    return X_STATUS_UNSUCCESSFUL;
  }

  auto* memory = StateOf(*runtime).GetMemory();
  auto header = memory->TranslateVirtual<const XEXHeader*>(user.guest_xex_header);
  if (!header) {
    return X_STATUS_UNSUCCESSFUL;
  }
  return GetOptHeader(memory, header, key, out_header_guest_ptr);
}

X_STATUS ModuleObject::GetOptHeader(const kernel::Memory* memory, const XEXHeader* header,
                                    XEXHeaderKey key, u32* out_header_guest_ptr) {
  REX_ASSERT(out_header_guest_ptr != nullptr);
  const KernelServices* services = GetKernelServices();
  if (!services) {
    return X_STATUS_UNSUCCESSFUL;
  }
  const auto entry = services->file_system->FindXEXOptHeader(
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

}  // namespace rex::kernel
