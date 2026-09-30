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

#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/elf_module.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xex_module.h>
#include <rex/system/xfile.h>
#include <rex/system/xthread.h>

namespace rex::system {

UserModule::UserModule(KernelState* kernel_state)
    : XModule(kernel_state, ModuleType::kUserModule) {}

UserModule::~UserModule() {
  Unload();
}

uint32_t UserModule::title_id() const {
  if (module_format_ != kModuleFormatXex) {
    return 0;
  }
  auto header = xex_header();
  for (uint32_t i = 0; i < header->header_count; i++) {
    auto& opt_header = header->headers[i];
    if (opt_header.key == XEX_HEADER_EXECUTION_INFO) {
      auto opt_header_ptr = reinterpret_cast<const uint8_t*>(header) + opt_header.offset;
      auto opt_exec_info = reinterpret_cast<const xex2_opt_execution_info*>(opt_header_ptr);
      return static_cast<uint32_t>(opt_exec_info->title_id);
    }
  }
  return 0;
}

X_STATUS UserModule::LoadFromFile(const std::string_view path) {
  X_STATUS result = X_STATUS_UNSUCCESSFUL;

  auto* fs = kernel_state_->file_system();
  auto fs_entry = fs->Stat(path);
  if (!fs_entry) {
    REX_FAIL(system, X_STATUS_NO_SUCH_FILE, "File not found: {}", path);
  }

  path_ = fs_entry->absolute_path;
  name_ = rex::utf8_find_base_name_from_guest_path(path_);

  filesystem::OpenOptions options;
  options.access = filesystem::Access::kGenericRead;
  auto opened = fs->Open(path_, options);
  if (!opened) {
    return ToXStatus(opened.error());
  }
  if (auto view = opened->file->View()) {
    result = LoadFromMemory(view->data(), view->size());
  } else {
    std::vector<uint8_t> buffer(fs_entry->size);
    auto bytes_read = opened->file->Read(buffer, 0);
    if (!bytes_read) {
      return ToXStatus(bytes_read.error());
    }
    result = LoadFromMemory(buffer.data(), *bytes_read);
  }

  // Only XEX returns X_STATUS_PENDING
  if (result != X_STATUS_PENDING) {
    return result;
  }

  // Search for sibling XEX patch file
  if (auto patch_entry = fs->Stat(path_ + "p")) {
    auto patch_path = patch_entry->absolute_path;

    REXLOG_DEBUG(system, "Loading XEX patch from {}", patch_path);

    auto patch_module = object_ref<UserModule>(new UserModule(kernel_state_));
    result = patch_module->LoadFromFile(patch_path);
    if (!result) {
      result = patch_module->xex_module()->ApplyPatch(xex_module());
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

  return LoadXexContinue();
}

X_STATUS UserModule::LoadFromMemory(const void* addr, const size_t length) {
  auto* dispatcher = kernel_state_->function_dispatcher();

  // Detect format by magic bytes
  be<memory::fourcc_t> magic;
  magic.value = memory::load<memory::fourcc_t>(addr);

  if (magic == runtime::kXEX2Signature || magic == runtime::kXEX1Signature) {
    module_format_ = kModuleFormatXex;
  } else if (magic == runtime::kElfSignature) {
    module_format_ = kModuleFormatElf;
  } else {
    REX_FAIL(system, X_STATUS_NOT_IMPLEMENTED, "Unknown module magic: {:08X}", uint32_t(magic));
  }

  if (module_format_ == kModuleFormatXex) {
    // Create XexModule to parse and load the XEX image into guest memory
    auto xex_module = new runtime::XexModule(dispatcher, kernel_state_);
    if (!xex_module->Load(name_, path_, addr, length)) {
      delete xex_module;
      return X_STATUS_UNSUCCESSFUL;
    }

    processor_module_ = xex_module;

    // Continue to LoadXexContinue (returns X_STATUS_PENDING per Xenia convention)
    return X_STATUS_PENDING;

  } else if (module_format_ == kModuleFormatElf) {
    // Create ElfModule to parse and load the ELF image into guest memory
    auto elf_module = new runtime::ElfModule(dispatcher, kernel_state_);
    if (!elf_module->Load(name_, path_, addr, length)) {
      delete elf_module;
      return X_STATUS_UNSUCCESSFUL;
    }

    entry_point_ = elf_module->entry_point();
    stack_size_ = 1024 * 1024;  // 1 MB default stack
    is_dll_module_ = false;

    processor_module_ = elf_module;
    OnLoad();
    return X_STATUS_SUCCESS;  // ELF doesn't need LoadXexContinue
  }

  return X_STATUS_UNSUCCESSFUL;
}

X_STATUS UserModule::LoadXexContinue() {
  // LoadXexContinue: finishes loading XEX after a patch has been applied (or
  // patch wasn't found)

  if (!this->xex_module()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  // If guest_xex_header is set we must have already loaded the XEX
  if (guest_xex_header_) {
    return X_STATUS_SUCCESS;
  }

  // Finish XexModule load (PE sections/imports/symbols...)
  if (!xex_module()->LoadContinue()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  // Copy the xex2 header into guest memory.
  auto header = this->xex_module()->xex_header();
  auto security_header = this->xex_module()->xex_security_info();
  guest_xex_header_ = memory()->SystemHeapAlloc(header->header_size);

  uint8_t* xex_header_ptr = memory()->TranslateVirtual(guest_xex_header_);
  std::memcpy(xex_header_ptr, header, header->header_size);

  // Cache some commonly used headers...
  this->xex_module()->GetOptHeader(XEX_HEADER_ENTRY_POINT, &entry_point_);
  this->xex_module()->GetOptHeader(XEX_HEADER_DEFAULT_STACK_SIZE, &stack_size_);
  is_dll_module_ = !!(header->module_flags & XEX_MODULE_DLL_MODULE);

  // Setup the loader data entry
  auto ldr_data = memory()->TranslateVirtual<X_LDR_DATA_TABLE_ENTRY*>(hmodule_ptr_);

  ldr_data->dll_base = 0;  // GetProcAddress will read this.
  ldr_data->xex_header_base = guest_xex_header_;
  ldr_data->full_image_size = security_header->image_size;
  ldr_data->image_base = this->xex_module()->base_address();
  ldr_data->entry_point = entry_point_;

  OnLoad();

  return X_STATUS_SUCCESS;
}

X_STATUS UserModule::Unload() {
  if (module_format_ == kModuleFormatXex && (!processor_module_ || !xex_module()->loaded())) {
    // Quick abort.
    return X_STATUS_SUCCESS;
  }

  if (module_format_ == kModuleFormatXex && processor_module_ && xex_module()->Unload()) {
    OnUnload();
    return X_STATUS_SUCCESS;
  }

  return X_STATUS_UNSUCCESSFUL;
}

uint32_t UserModule::GetProcAddressByOrdinal(uint16_t ordinal, uint32_t caller_address) {
  uint32_t guest_addr = xex_module()->GetProcAddress(ordinal);
  if (!guest_addr || !caller_address) {
    return guest_addr;
  }

  auto* dispatcher = kernel_state_->function_dispatcher();
  auto* func = dispatcher->GetFunction(guest_addr);
  if (!func) {
    return guest_addr;
  }

  return dispatcher->AllocateThunk(func, caller_address);
}

uint32_t UserModule::GetProcAddressByName(std::string_view name) {
  return xex_module()->GetProcAddress(name);
}

X_STATUS UserModule::GetSection(const std::string_view name, uint32_t* out_section_data,
                                uint32_t* out_section_size) {
  xex2_opt_resource_info* resource_header = nullptr;
  if (!runtime::XexModule::GetOptHeader(xex_header(), XEX_HEADER_RESOURCE_INFO, &resource_header)) {
    // No resources.
    return X_STATUS_NOT_FOUND;
  }
  uint32_t count = (resource_header->size - 4) / sizeof(xex2_resource);
  for (uint32_t i = 0; i < count; i++) {
    auto& res = resource_header->resources[i];
    if (rex::utf8_equal_z(name, std::string_view(res.name, 8))) {
      // Found!
      *out_section_data = res.address;
      *out_section_size = res.size;
      return X_STATUS_SUCCESS;
    }
  }

  return X_STATUS_NOT_FOUND;
}

X_STATUS UserModule::GetOptHeader(xex2_header_keys key, void** out_ptr) {
  assert_not_null(out_ptr);

  if (module_format_ == kModuleFormatElf) {
    // Quick die.
    return X_STATUS_UNSUCCESSFUL;
  }

  bool ret = xex_module()->GetOptHeader(key, out_ptr);
  if (!ret) {
    return X_STATUS_NOT_FOUND;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS UserModule::GetOptHeader(xex2_header_keys key, uint32_t* out_header_guest_ptr) {
  if (module_format_ == kModuleFormatElf) {
    // Quick die.
    return X_STATUS_UNSUCCESSFUL;
  }

  auto header = memory()->TranslateVirtual<const xex2_header*>(guest_xex_header_);
  if (!header) {
    return X_STATUS_UNSUCCESSFUL;
  }
  return GetOptHeader(memory(), header, key, out_header_guest_ptr);
}

X_STATUS UserModule::GetOptHeader(const memory::Memory* memory, const xex2_header* header,
                                  xex2_header_keys key, uint32_t* out_header_guest_ptr) {
  assert_not_null(out_header_guest_ptr);
  uint32_t field_value = 0;
  bool field_found = false;
  for (uint32_t i = 0; i < header->header_count; i++) {
    auto& opt_header = header->headers[i];
    if (opt_header.key != key) {
      continue;
    }
    field_found = true;
    switch (opt_header.key & 0xFF) {
      case 0x00:
        // Return data stored in header value.
        field_value = opt_header.value;
        break;
      case 0x01:
        // Return pointer to data stored in header value.
        field_value = memory->HostToGuestVirtual(&opt_header.value);
        break;
      default:
        // Data stored at offset to header.
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

void UserModule::Dump() {
  // TODO(tomc): do we need this?
}

}  // namespace rex::system
