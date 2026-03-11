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
#include <rex/stream.h>
#include <rex/system/elf_module.h>
#include <rex/system/kernel_state.h>
#include <rex/system/processor.h>
#include <rex/system/user_module.h>
#include <rex/system/xex_module.h>
#include <rex/system/xfile.h>
#include <rex/system/xthread.h>

REXCVAR_DEFINE_BOOL(xex_apply_patches, true, "Kernel", "Apply XEX patches");

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

  // Resolve the file to open.
  // TODO(benvanik): make this code shared?
  auto fs_entry = kernel_state()->file_system()->ResolvePath(path);
  if (!fs_entry) {
    REXSYS_ERROR("File not found: {}", path);
    return X_STATUS_NO_SUCH_FILE;
  }

  path_ = fs_entry->absolute_path();
  name_ = rex::string::utf8_find_base_name_from_guest_path(path_);

  // If the FS supports mapping, map the file in and load from that.
  if (fs_entry->can_map()) {
    // Map.
    auto mmap = fs_entry->OpenMapped(memory::MappedMemory::Mode::kRead);
    if (!mmap) {
      return result;
    }

    // Load the module.
    result = LoadFromMemory(mmap->data(), mmap->size());
  } else {
    std::vector<uint8_t> buffer(fs_entry->size());

    // Open file for reading.
    rex::filesystem::File* file = nullptr;
    result = fs_entry->Open(rex::filesystem::FileAccess::kGenericRead, &file);
    if (XFAILED(result)) {
      return result;
    }

    // Read entire file into memory.
    // Ugh.
    size_t bytes_read = 0;
    result = file->ReadSync(std::span<uint8_t>(buffer), 0, &bytes_read);
    if (XFAILED(result)) {
      return result;
    }

    // Load the module.
    result = LoadFromMemory(buffer.data(), bytes_read);

    // Close the file.
    file->Destroy();
  }

  // Only XEX returns X_STATUS_PENDING
  if (result != X_STATUS_PENDING) {
    return result;
  }

  if (REXCVAR_GET(xex_apply_patches)) {
    // Search for xexp patch file
    auto patch_entry = kernel_state()->file_system()->ResolvePath(path_ + "p");

    if (patch_entry) {
      auto patch_path = patch_entry->absolute_path();

      REXSYS_DEBUG("Loading XEX patch from {}", patch_path);

      auto patch_module = object_ref<UserModule>(new UserModule(kernel_state_));
      result = patch_module->LoadFromFile(patch_path);
      if (!result) {
        result = patch_module->xex_module()->ApplyPatch(xex_module());
        if (result) {
          REXSYS_ERROR("Failed to apply XEX patch, code: {}", result);
        }
      } else {
        REXSYS_ERROR("Failed to load XEX patch, code: {}", result);
      }

      if (result) {
        return X_STATUS_UNSUCCESSFUL;
      }
    }
  }

  return LoadXexContinue();
}

X_STATUS UserModule::LoadFromMemory(const void* addr, const size_t length) {
  auto processor = kernel_state()->processor();

  // Detect format by magic bytes
  be<memory::fourcc_t> magic;
  magic.value = memory::load<memory::fourcc_t>(addr);

  if (magic == runtime::kXEX2Signature || magic == runtime::kXEX1Signature) {
    module_format_ = kModuleFormatXex;
  } else if (magic == runtime::kElfSignature) {
    module_format_ = kModuleFormatElf;
  } else {
    REXSYS_ERROR("Unknown module magic: {:08X}", uint32_t(magic));
    return X_STATUS_NOT_IMPLEMENTED;
  }

  if (module_format_ == kModuleFormatXex) {
    // Create XexModule to parse and load the XEX image into guest memory
    auto xex_module = new runtime::XexModule(processor, kernel_state());
    if (!xex_module->Load(name_, path_, addr, length)) {
      delete xex_module;
      return X_STATUS_UNSUCCESSFUL;
    }

    processor_module_ = xex_module;

    // Continue to LoadXexContinue (returns X_STATUS_PENDING per Xenia convention)
    return X_STATUS_PENDING;

  } else if (module_format_ == kModuleFormatElf) {
    // Create ElfModule to parse and load the ELF image into guest memory
    auto elf_module = new runtime::ElfModule(processor, kernel_state());
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

uint32_t UserModule::GetProcAddressByOrdinal(uint16_t ordinal) {
  return xex_module()->GetProcAddress(ordinal);
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
    if (rex::string::utf8_equal_z(name, std::string_view(res.name, 8))) {
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

bool UserModule::Save(stream::ByteStream* stream) {
  if (!XModule::Save(stream)) {
    return false;
  }

  // A lot of the information stored on this class can be reconstructed at
  // runtime.

  return true;
}

object_ref<UserModule> UserModule::Restore(KernelState* kernel_state, stream::ByteStream* stream,
                                           const std::string_view path) {
  auto module = new UserModule(kernel_state);

  // XModule::Save took care of this earlier...
  // TODO: Find a nicer way to represent that here.
  if (!module->RestoreObject(stream)) {
    return nullptr;
  }

  auto result = module->LoadFromFile(path);
  if (XFAILED(result)) {
    REXSYS_DEBUG("UserModule::Restore LoadFromFile({}) FAILED - code {:08X}", path, result);
    return nullptr;
  }

  if (!kernel_state->RegisterUserModule(retain_object(module))) {
    // Already loaded?
    assert_always();
  }

  return object_ref<UserModule>(module);
}

void UserModule::Dump() {
  // TODO(tomc): do we need this?
}

}  // namespace rex::system
