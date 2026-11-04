/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <utility>

#include <format>

#include <rex/filesystem/path.h>
#include <rex/filesystem/xex.h>
#include <rex/kernel/types/xex.h>
#include <rex/logging/log_category.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/kernel/export.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/kernel/module.h>
#include <rex/kernel/object/object_pin.h>
#include <rex/kernel/object/module_object.h>
#include <rex/types.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/runtime_settings.h>
#include <rex/kernel/object/object_table.h>
#include <rex/filesystem/file_system_factory.h>
#include "kernel/rexkernel_module.h"
#include "kernel/settings_access.h"

namespace rex::kernel {

XEXModule::XEXModule(FunctionDispatcher* function_dispatcher, TitleRuntime& runtime)
    : GuestModule(function_dispatcher), runtime_(&runtime) {}

XEXModule::~XEXModule() = default;

u32 XEXModule::GetImageSize() const {
  REX_ASSERT(base_address_ != 0);
  const u32 page_size = GetAddressSpace()->LookupHeap(base_address_)->GetPageSize();
  u32 total = 0;
  for (const auto& range : image_->GetSecurity().pages) {
    total += range.page_count * page_size;
  }
  return total;
}

u32 XEXModule::GetExportAddress(u16 ordinal) const {
  if (const u32 table_address = image_->GetSecurity().export_table) {
    auto export_table = GetAddressSpace()->TranslateVirtual<const XEXExportTable*>(table_address);
    ordinal -= export_table->base;
    if (ordinal >= export_table->count) {
      REXSYS_FAIL(0, "GetProcAddress({:03X}): ordinal out of bounds", ordinal);
    }
    u32 ordinal_offset = export_table->ordinal_offset[ordinal];
    ordinal_offset += export_table->image_base_address << 16;
    return ordinal_offset;
  }

  REX_ASSERT(base_address_ != 0);
  if (const auto* directory = image_->GetOptHeader<XEXOptDataDirectory>(kXEXHeaderExportsByName)) {
    auto e = GetAddressSpace()->TranslateVirtual<const X_IMAGE_EXPORT_DIRECTORY*>(
        base_address_ + directory->offset);
    REX_ASSERT(e != nullptr);
    const auto* bytes = reinterpret_cast<const u8*>(e);
    auto* function_table = reinterpret_cast<const u32*>(bytes + e->AddressOfFunctions);
    if (ordinal < e->NumberOfFunctions) {
      return base_address_ + function_table[ordinal];
    }
  }
  return 0;
}

u32 XEXModule::GetExportAddress(const std::string_view name) const {
  REX_ASSERT(base_address_ != 0);
  const auto* directory = image_->GetOptHeader<XEXOptDataDirectory>(kXEXHeaderExportsByName);
  if (!directory) {
    return 0;
  }
  auto e = GetAddressSpace()->TranslateVirtual<const X_IMAGE_EXPORT_DIRECTORY*>(base_address_ +
                                                                                directory->offset);
  REX_ASSERT(e != nullptr);
  const auto* bytes = reinterpret_cast<const u8*>(e);
  auto* function_table = reinterpret_cast<const u32*>(bytes + e->AddressOfFunctions);
  auto* name_table = reinterpret_cast<const u32*>(bytes + e->AddressOfNames);
  auto* ordinal_table = reinterpret_cast<const u16*>(bytes + e->AddressOfNameOrdinals);
  for (u32 i = 0; i < e->NumberOfNames; i++) {
    auto fn_name = reinterpret_cast<const char*>(bytes + name_table[i]);
    if (name == std::string_view(fn_name)) {
      return base_address_ + function_table[ordinal_table[i]];
    }
  }
  return 0;
}

int XEXModule::ApplyPatch(XEXModule* module) {
  if (!image_->IsPatch()) {
    return 1;
  }
  const u32 original_image_size = module->GetImageSize();
  auto patched = module->image_->ApplyHeaderDelta(*image_);
  if (!patched) {
    REXSYS_FAIL(2, "XEX header patch failed, {}", patched.error().GetDescription());
  }
  module->image_ = std::move(*patched);
  const u32 new_image_size = module->GetImageSize();

  if (new_image_size > original_image_size) {
    const u32 size_delta = new_image_size - original_image_size;
    const u32 addr_new_mem = module->base_address_ + original_image_size;
    const bool alloc_result = GetAddressSpace()
                                  ->LookupHeap(addr_new_mem)
                                  ->AllocFixed(addr_new_mem, size_delta, 4096,
                                               kMemoryAllocationReserve | kMemoryAllocationCommit |
                                                   kMemoryAllocationTitle,
                                               kMemoryProtectRead | kMemoryProtectWrite);
    if (!alloc_result) {
      REXSYS_FAIL(6, "Unable to allocate XEX memory at {:08X} with size {:08X}.", addr_new_mem,
                  size_delta);
    }
  }

  REX_ASSERT(!module->finished_load_);
  auto& host_image = module->pending_image_;
  host_image.resize(
      std::max<size_t>(host_image.size(), std::max(original_image_size, new_image_size)));
  std::span<u8> base_image(host_image.data(), std::max(original_image_size, new_image_size));
  if (auto applied = module->image_->ApplyImageDelta(*image_, base_image); !applied) {
    REXSYS_FAIL(9, "XEX patch application failed, {}", applied.error().GetDescription());
  }

  host_image.resize(new_image_size);

  if (original_image_size > new_image_size) {
    const u32 size_delta = original_image_size - new_image_size;
    const u32 addr_free_mem = module->base_address_ + new_image_size;
    if (!GetAddressSpace()->LookupHeap(addr_free_mem)->Decommit(addr_free_mem, size_delta)) {
      REXSYS_ERROR("Unable to decommit XEX memory at {:08X} with size {:08X}.", addr_free_mem,
                   size_delta);
    }
  }

  const auto* descriptor =
      image_->GetOptHeader<XEXOptDeltaPatchDescriptor>(kXEXHeaderDeltaPatchDescriptor);
  const auto source = descriptor->GetSourceVersion();
  const auto target = descriptor->GetTargetVersion();
  REXSYS_INFO(
      "XEX patch applied successfully, base version {}.{}.{}.{}, new version "
      "{}.{}.{}.{}",
      u32{source.major}, u32{source.minor}, u32{source.build}, u32{source.qfe}, u32{target.major},
      u32{target.minor}, u32{target.build}, u32{target.qfe});
  return 0;
}

bool XEXModule::Load(const std::string_view name, const std::string_view path, const void* xex_addr,
                     size_t xex_length) {
  const std::span<const u8> file(static_cast<const u8*>(xex_addr), xex_length);
  const KernelServices* services = GetKernelServices();
  if (!services) {
    return false;
  }
  auto parsed = services->file_system->ParseXEX(file);
  if (!parsed) {
    return false;
  }
  REX_ASSERT(!loaded_);
  loaded_ = true;
  image_ = std::move(*parsed);

  base_address_ = image_->GetOptHeaderValue(kXEXHeaderImageBaseAddress)
                      .value_or(image_->GetSecurity().load_address);
  name_ = name;
  path_ = path;

  if (image_->IsPatch()) {
    return true;
  }

  const auto* format = image_->GetOptHeader<XEXOptFileFormatInfo>(kXEXHeaderFileFormatInfo);
  if (!format) {
    REXSYS_FAIL(false, "XEX has no file format header");
  }
  const u32 alloc_size = static_cast<u16>(format->compression_type) == kXEXCompressionNone
                             ? static_cast<u32>(xex_length - image_->GetHeaderBytes().size())
                             : GetImageSize();

  auto* heap = GetAddressSpace()->LookupHeap(base_address_);
  HeapAllocation occupant{};
  if (!heap->QueryRegionInfo(base_address_, &occupant) || occupant.state != 0 ||
      occupant.region_size < rex::RoundUpPow2(alloc_size, heap->GetPageSize())) {
    REXSYS_FAIL(false,
                "XEX image range {:08X}-{:08X} is in use, the module holding it must be "
                "unloaded first.",
                base_address_, base_address_ + alloc_size);
  }
  if (!heap->AllocFixed(base_address_, alloc_size, 4096,
                        kMemoryAllocationReserve | kMemoryAllocationCommit | kMemoryAllocationTitle,
                        kMemoryProtectRead | kMemoryProtectWrite)) {
    REXSYS_FAIL(false, "Unable to allocate XEX memory at {:08X} with size {:08X}.", base_address_,
                alloc_size);
  }
  pending_image_.assign(alloc_size, 0);
  std::span<u8> buffer(pending_image_);
  if (auto decoded = image_->Decode(file, buffer); !decoded) {
    REXSYS_FAIL(false, "XEX load failed, tried both encryption keys, {}",
                decoded.error().GetDescription());
  }
  if (image_->GetKey() != filesystem::XEXKey::kRetail) {
    REXSYS_WARN("XEX decoded with a non retail key ({})", static_cast<u32>(image_->GetKey()));
  }
  return true;
}

bool XEXModule::LoadContinue() {
  REX_ASSERT(!finished_load_);
  if (finished_load_) {
    return true;
  }
  finished_load_ = true;
  if (image_->IsPatch()) {
    return true;
  }

  auto* heap = GetAddressSpace()->LookupHeap(base_address_);
  const u32 page_size = heap->GetPageSize();
  const u32 image_end = static_cast<u32>(std::min<size_t>(pending_image_.size(), GetImageSize()));
  auto parsed_pe = GetKernelServices()->file_system->ParsePE(
      std::span<const u8>(pending_image_.data(), image_end));
  if (!parsed_pe) {
    REXSYS_FAIL(false, "Failed to load XEX PE headers, {}", parsed_pe.error().GetDescription());
  }
  pe_ = std::move(*parsed_pe);
  if (pe_.header_size < image_end) {
    std::memcpy(GetAddressSpace()->TranslateVirtual(base_address_ + pe_.header_size),
                pending_image_.data() + pe_.header_size, image_end - pe_.header_size);
  }
  std::vector<u8>().swap(pending_image_);
  for (auto& section : pe_.sections) {
    section.address += base_address_;
  }

  for (const auto& library : image_->GetImports()) {
    SetupLibraryImports(library.name, library);
  }

  u32 page = 0;
  for (const auto& range : image_->GetSecurity().pages) {
    const u32 address = base_address_ + page * page_size;
    const u32 size = range.page_count * page_size;
    switch (range.type) {
      case kXEXSectionCode:
      case kXEXSectionReadOnlyData:
        heap->Protect(address, size,
                      GetRuntimeSettings().writable_code_segments.Get()
                          ? kMemoryProtectRead | kMemoryProtectWrite
                          : kMemoryProtectRead);
        break;
      case kXEXSectionData:
        heap->Protect(address, size, kMemoryProtectRead | kMemoryProtectWrite);
        break;
    }
    page += range.page_count;
  }

  PopulateBinaryData();
  return true;
}

bool XEXModule::Unload() {
  if (!loaded_) {
    return true;
  }
  loaded_ = false;
  if (!image_->IsPatch()) {
    REX_ASSERT(base_address_ != 0);
    GetAddressSpace()->LookupHeap(base_address_)->Release(base_address_);
  }
  image_.reset();
  std::vector<u8>().swap(pending_image_);
  return true;
}

bool XEXModule::SetupLibraryImports(std::string_view name,
                                    const filesystem::XEXImportLibrary& library) {
  auto base_name = rex::filesystem::utf8_find_base_name_from_guest_path(name);

  struct KernelModule {
    rex::kernel::ExportModule exports;
    kernel::Pinned<kernel::ModuleObject> object;
  };
  std::optional<KernelModule> kernel;
  if (const auto exports = rex::kernel::ModuleFromName(base_name);
      exports && runtime_->GetModuleRegistry().IsKernelModule(base_name)) {
    if (auto object = runtime_->GetObjectTable().Get<ModuleObject>(
            runtime_->GetModuleRegistry().FindKernelModule(base_name).value_or(0))) {
      kernel.emplace(*exports, std::move(object));
    }
  }

  ImportLibrary import_library;
  import_library.name = base_name;
  import_library.file_name = std::string(name);
  import_library.id = library.id;
  import_library.version = library.version;
  import_library.min_version = library.min_version;

  std::unordered_map<u16, LibraryImport> import_map;
  for (const u32 record_addr : library.records) {
    if (!record_addr) {
      continue;
    }
    auto record_slot = GetAddressSpace()->TranslateVirtual<be_u32*>(record_addr);
    const u32 record_value = *record_slot;
    const u16 record_type = (record_value & 0xFF000000) >> 24;
    const u16 ordinal = record_value & 0xFFFF;

    auto& import_entry = import_map[ordinal];
    import_entry.ordinal = ordinal;
    if (record_type == 0) {
      import_entry.value_address = record_addr;
      if (kernel) {
        const auto* kernel_export = rex::kernel::FindByOrdinal(kernel->exports, ordinal);
        if (kernel_export && kernel_export->type == rex::kernel::ExportType::kVariable) {
          const u32 variable_ptr = kernel->object->GetVariable(ordinal);
          if (variable_ptr) {
            *record_slot = variable_ptr;
            REXSYS_DEBUG("Patched variable import {}:{:#x} ({}) -> {:#x}", base_name, ordinal,
                         kernel_export->name, variable_ptr);
          } else {
            *record_slot = 0xD000BEEF | (kernel_export->ordinal & 0xFFF) << 16;
            REXSYS_WARN("Variable import {}:{:#x} ({}) not implemented", base_name, ordinal,
                        kernel_export->name);
          }
        }
      }
    } else if (record_type == 1) {
      import_entry.thunk_address = record_addr;
    }
  }

  std::vector<u16> ordinals;
  ordinals.reserve(import_map.size());
  for (const auto& [ordinal, import_info] : import_map) {
    ordinals.push_back(ordinal);
    if (!kernel || !import_info.value_address || !import_info.thunk_address) {
      continue;
    }
    const auto* kernel_export = rex::kernel::FindByOrdinal(kernel->exports, ordinal);
    if (kernel_export && kernel_export->type == rex::kernel::ExportType::kFunction) {
      *GetAddressSpace()->TranslateVirtual<be_u32*>(import_info.value_address) =
          import_info.thunk_address;
      REXSYS_DEBUG("Patched function import {}:{:#x} ({}) -> {:#x}", base_name, ordinal,
                   kernel_export->name, import_info.thunk_address);
    }
  }
  std::ranges::sort(ordinals);
  for (const u16 ordinal : ordinals) {
    import_library.imports.push_back(import_map[ordinal]);
  }

  import_libs_.push_back(import_library);
  REXSYS_DEBUG("created symbols for import library {} with {} imports", base_name,
               import_library.imports.size());
  return true;
}

void XEXModule::PopulateBinaryData() {
  binary_sections_.clear();
  binary_symbols_.clear();
  for (const auto& pe_sec : pe_.sections) {
    BinarySection sec;
    sec.name = pe_sec.name;
    sec.virtual_address = pe_sec.address;
    sec.virtual_size = pe_sec.size;
    sec.host_data = GetAddressSpace()->TranslateVirtual<u8*>(pe_sec.address);
    sec.executable = (pe_sec.flags & filesystem::kPESectionMemoryExecute) != 0;
    sec.writable = (pe_sec.flags & filesystem::kPESectionMemoryWrite) != 0;
    binary_sections_.push_back(std::move(sec));
  }
  for (const auto& lib : import_libs_) {
    for (const auto& import : lib.imports) {
      BinarySymbol sym;
      sym.name = std::format("{}@{}", lib.name, import.ordinal);
      sym.address = import.thunk_address;
      sym.size = 16;
      sym.type = BinarySymbolType::kImport;
      binary_symbols_.push_back(std::move(sym));
    }
  }
}

std::span<const BinarySection> XEXModule::GetBinarySections() const {
  return binary_sections_;
}

std::span<const BinarySymbol> XEXModule::GetBinarySymbols() const {
  return binary_symbols_;
}

}  // namespace rex::kernel
