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
#include <unordered_map>

#include <format>

#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/runtime.h>
#include <rex/runtime/exports.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime/kernel_module.h>
#include <rex/runtime/xex_module.h>
#include <rex/runtime/xmodule.h>
#include <rex/types.h>
#include "runtime/cvar_table.h"

namespace rex::runtime {

XexModule::XexModule(FunctionDispatcher* function_dispatcher, Runtime& runtime)
    : Module(function_dispatcher), runtime_(&runtime) {}

XexModule::~XexModule() = default;

u32 XexModule::image_size() const {
  REX_ASSERT(base_address_ != 0);
  const u32 page_size = memory()->LookupHeap(base_address_)->page_size();
  u32 total = 0;
  for (const auto& range : image_.security().pages) {
    total += range.page_count * page_size;
  }
  return total;
}

u32 XexModule::GetProcAddress(u16 ordinal) const {
  if (const u32 table_address = image_.security().export_table) {
    auto export_table = memory()->TranslateVirtual<const xex2_export_table*>(table_address);
    ordinal -= export_table->base;
    if (ordinal >= export_table->count) {
      REX_FAIL(system, 0, "GetProcAddress({:03X}): ordinal out of bounds", ordinal);
    }
    u32 ordinal_offset = export_table->ordOffset[ordinal];
    ordinal_offset += export_table->imagebaseaddr << 16;
    return ordinal_offset;
  }

  REX_ASSERT(base_address_ != 0);
  if (const auto* directory =
          image_.OptHeader<xex2_opt_data_directory>(XEX_HEADER_EXPORTS_BY_NAME)) {
    auto e = memory()->TranslateVirtual<const X_IMAGE_EXPORT_DIRECTORY*>(base_address_ +
                                                                         directory->offset);
    REX_ASSERT(e != nullptr);
    const auto* bytes = reinterpret_cast<const u8*>(e);
    auto* function_table = reinterpret_cast<const u32*>(bytes + e->AddressOfFunctions);
    if (ordinal < e->NumberOfFunctions) {
      return base_address_ + function_table[ordinal];
    }
  }
  return 0;
}

u32 XexModule::GetProcAddress(const std::string_view name) const {
  REX_ASSERT(base_address_ != 0);
  const auto* directory = image_.OptHeader<xex2_opt_data_directory>(XEX_HEADER_EXPORTS_BY_NAME);
  if (!directory) {
    return 0;
  }
  auto e = memory()->TranslateVirtual<const X_IMAGE_EXPORT_DIRECTORY*>(base_address_ +
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

int XexModule::ApplyPatch(XexModule* module) {
  if (!image_.is_patch()) {
    return 1;
  }
  const u32 original_image_size = module->image_size();
  auto patched = module->image_.ApplyHeaderDelta(image_);
  if (!patched) {
    REX_FAIL(system, 2, "XEX header patch failed: {}", patched.error().what());
  }
  module->image_ = std::move(*patched);
  const u32 new_image_size = module->image_size();

  if (new_image_size > original_image_size) {
    const u32 size_delta = new_image_size - original_image_size;
    const u32 addr_new_mem = module->base_address_ + original_image_size;
    const bool alloc_result = memory()
                                  ->LookupHeap(addr_new_mem)
                                  ->AllocFixed(addr_new_mem, size_delta, 4096,
                                               kMemoryAllocationReserve | kMemoryAllocationCommit |
                                                   kMemoryAllocationTitle,
                                               kMemoryProtectRead | kMemoryProtectWrite);
    if (!alloc_result) {
      REX_FAIL(system, 6, "Unable to allocate XEX memory at {:08X}-{:08X}.", addr_new_mem,
               size_delta);
    }
  }

  std::span<u8> base_image(memory()->TranslateVirtual(module->base_address_),
                           std::max(original_image_size, new_image_size));
  if (auto applied = module->image_.ApplyImageDelta(image_, base_image); !applied) {
    REX_FAIL(system, 9, "XEX patch application failed: {}", applied.error().what());
  }

  if (original_image_size > new_image_size) {
    const u32 size_delta = original_image_size - new_image_size;
    const u32 addr_free_mem = module->base_address_ + new_image_size;
    if (!memory()->LookupHeap(addr_free_mem)->Decommit(addr_free_mem, size_delta)) {
      REXLOG_ERROR(system, "Unable to decommit XEX memory at {:08X}-{:08X}.", addr_free_mem,
                   size_delta);
    }
  }

  const auto* descriptor =
      image_.OptHeader<xex2_opt_delta_patch_descriptor>(XEX_HEADER_DELTA_PATCH_DESCRIPTOR);
  const auto source = descriptor->source_version();
  const auto target = descriptor->target_version();
  REXLOG_INFO(system,
              "XEX patch applied successfully: base version: {}.{}.{}.{}, new version: "
              "{}.{}.{}.{}",
              u32{source.major}, u32{source.minor}, u32{source.build}, u32{source.qfe},
              u32{target.major}, u32{target.minor}, u32{target.build}, u32{target.qfe});
  return 0;
}

bool XexModule::Load(const std::string_view name, const std::string_view path, const void* xex_addr,
                     size_t xex_length) {
  const std::span<const u8> file(static_cast<const u8*>(xex_addr), xex_length);
  auto parsed = filesystem::XEXImage::Parse(file);
  if (!parsed) {
    return false;
  }
  REX_ASSERT(!loaded_);
  loaded_ = true;
  image_ = std::move(*parsed);

  base_address_ =
      image_.OptHeaderValue(XEX_HEADER_IMAGE_BASE_ADDRESS).value_or(image_.security().load_address);
  name_ = name;
  path_ = path;

  if (image_.is_patch()) {
    return true;
  }

  const auto* format = image_.OptHeader<xex2_opt_file_format_info>(XEX_HEADER_FILE_FORMAT_INFO);
  if (!format) {
    REX_FAIL(system, false, "XEX has no file format header");
  }
  const u32 alloc_size = static_cast<u16>(format->compression_type) == XEX_COMPRESSION_NONE
                             ? static_cast<u32>(xex_length - image_.header_bytes().size())
                             : image_size();

  auto* heap = memory()->LookupHeap(base_address_);
  if (!heap->AllocFixed(base_address_, alloc_size, 4096,
                        kMemoryAllocationReserve | kMemoryAllocationCommit | kMemoryAllocationTitle,
                        kMemoryProtectRead | kMemoryProtectWrite)) {
    REX_FAIL(system, false, "Unable to allocate XEX memory at {:08X}-{:08X}.", base_address_,
             alloc_size);
  }
  std::span<u8> buffer(memory()->TranslateVirtual(base_address_), alloc_size);
  if (auto decoded = image_.Decode(file, buffer); !decoded) {
    REX_FAIL(system, false, "XEX load failed, tried both encryption keys: {}",
             decoded.error().what());
  }
  if (image_.key() != filesystem::XEXKey::Retail) {
    REXLOG_WARN(system, "XEX decoded with a non retail key ({})", static_cast<u32>(image_.key()));
  }
  return true;
}

bool XexModule::LoadContinue() {
  REX_ASSERT(!finished_load_);
  if (finished_load_) {
    return true;
  }
  finished_load_ = true;

  auto* heap = memory()->LookupHeap(base_address_);
  const u32 page_size = heap->page_size();
  auto parsed_pe = filesystem::ParsePE(
      std::span<const u8>(memory()->TranslateVirtual(base_address_), image_size()));
  if (!parsed_pe) {
    REX_FAIL(system, false, "Failed to load XEX PE headers: {}", parsed_pe.error().what());
  }
  pe_ = std::move(*parsed_pe);
  for (auto& section : pe_.sections) {
    section.address += base_address_;
  }

  for (const auto& library : image_.imports()) {
    SetupLibraryImports(library.name, library);
  }

  u32 page = 0;
  for (const auto& range : image_.security().pages) {
    const u32 address = base_address_ + page * page_size;
    const u32 size = range.page_count * page_size;
    switch (range.type) {
      case XEX_SECTION_CODE:
      case XEX_SECTION_READONLY_DATA:
        heap->Protect(address, size,
                      REXCVAR_GET(::rex::runtime::cvars::writable_code_segments)
                          ? kMemoryProtectRead | kMemoryProtectWrite
                          : kMemoryProtectRead);
        break;
      case XEX_SECTION_DATA:
        heap->Protect(address, size, kMemoryProtectRead | kMemoryProtectWrite);
        break;
    }
    page += range.page_count;
  }

  PopulateBinaryData();
  return true;
}

bool XexModule::Unload() {
  if (!loaded_) {
    return true;
  }
  loaded_ = false;
  if (!image_.is_patch()) {
    REX_ASSERT(base_address_ != 0);
    memory()->LookupHeap(base_address_)->Release(base_address_);
  }
  image_ = filesystem::XEXImage();
  return true;
}

bool XexModule::SetupLibraryImports(std::string_view name,
                                    const filesystem::XEXImportLibrary& library) {
  auto base_name = rex::utf8_find_base_name_from_guest_path(name);

  const auto kernel_module_id = rex::runtime::ModuleFromName(base_name);
  runtime::Pinned<runtime::ModuleObject> kernel_module;
  if (kernel_module_id && runtime_->module_registry().IsKernel(base_name)) {
    kernel_module = runtime_->object_table().Get<ModuleObject>(
        runtime_->module_registry().FindKernel(base_name).value_or(0));
  }

  ImportLibrary library_info;
  library_info.name = base_name;
  library_info.id = library.id;
  library_info.version = library.version;
  library_info.min_version = library.min_version;

  std::unordered_map<u16, ImportLibraryFn> import_map;
  for (const u32 record_addr : library.records) {
    if (!record_addr) {
      continue;
    }
    auto record_slot = memory()->TranslateVirtual<be_u32*>(record_addr);
    const u32 record_value = *record_slot;
    const u16 record_type = (record_value & 0xFF000000) >> 24;
    const u16 ordinal = record_value & 0xFFFF;

    auto& import_info = import_map[ordinal];
    import_info.ordinal = ordinal;
    if (record_type == 0) {
      import_info.value_address = record_addr;
      if (kernel_module) {
        const auto* kernel_export = rex::runtime::FindByOrdinal(*kernel_module_id, ordinal);
        if (kernel_export && kernel_export->kind == rex::runtime::ExportKind::Variable) {
          const u32 variable_ptr = runtime::KernelModuleVariable(*kernel_module, ordinal);
          if (variable_ptr) {
            *record_slot = variable_ptr;
            REXLOG_DEBUG(system, "Patched variable import {}:{:#x} ({}) -> {:#x}", base_name,
                         ordinal, kernel_export->name, variable_ptr);
          } else {
            *record_slot = 0xD000BEEF | (kernel_export->ordinal & 0xFFF) << 16;
            REXLOG_WARN(system, "Variable import {}:{:#x} ({}) not implemented", base_name, ordinal,
                        kernel_export->name);
          }
        }
      }
    } else if (record_type == 1) {
      import_info.thunk_address = record_addr;
    }
  }

  std::vector<u16> ordinals;
  ordinals.reserve(import_map.size());
  for (const auto& [ordinal, _] : import_map) {
    ordinals.push_back(ordinal);
  }
  std::ranges::sort(ordinals);
  for (const u16 ordinal : ordinals) {
    library_info.imports.push_back(import_map[ordinal]);
  }

  import_libs_.push_back(library_info);
  REXLOG_DEBUG(system, "created symbols for import library {} with {} imports", base_name,
               library_info.imports.size());
  return true;
}

void XexModule::PopulateBinaryData() {
  binary_sections_.clear();
  binary_symbols_.clear();
  for (const auto& pe_sec : pe_.sections) {
    BinarySection sec;
    sec.name = pe_sec.name;
    sec.virtual_address = pe_sec.address;
    sec.virtual_size = pe_sec.size;
    sec.host_data = memory()->TranslateVirtual<u8*>(pe_sec.address);
    sec.executable = (pe_sec.flags & kXEPESectionMemoryExecute) != 0;
    sec.writable = (pe_sec.flags & kXEPESectionMemoryWrite) != 0;
    binary_sections_.push_back(std::move(sec));
  }
  for (const auto& lib : import_libs_) {
    for (const auto& import : lib.imports) {
      BinarySymbol sym;
      sym.name = std::format("{}@{}", lib.name, import.ordinal);
      sym.address = import.thunk_address;
      sym.size = 16;
      sym.type = BinarySymbolType::Import;
      binary_symbols_.push_back(std::move(sym));
    }
  }
}

std::span<const BinarySection> XexModule::binary_sections() const {
  return binary_sections_;
}

std::span<const BinarySymbol> XexModule::binary_symbols() const {
  return binary_symbols_;
}

}  // namespace rex::runtime
