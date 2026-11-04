#include <limits>

#include "binary_view.h"
#include <rex/logging/log.h>
#include "codegen_categories.h"

#include <rex/kernel/symbol_map.h>
#include <rex/kernel/module.h>

namespace rex::codegen {

BinaryView BinaryView::FromModule(const kernel::Module& module) {
  BinaryView view;

  view.base_address_ = module.GetBaseAddress();
  view.image_size_ = module.GetImageSize();
  view.entry_point_ = module.GetEntryPoint();
  view.exception_directory_addr_ = module.GetExceptionDirectoryAddress();
  view.exception_directory_size_ = module.GetExceptionDirectorySize();
  view.export_table_addr_ = module.GetExportTableAddress();

  // Import thunk table and the export table after it extend to the end of .text
  u32 min_import_addr = std::numeric_limits<u32>::max();
  for (const auto& sym : module.GetBinarySymbols()) {
    if (sym.type == kernel::BinarySymbolType::kImport) {
      view.import_symbols_.push_back(ImportSymbol{.address = sym.address, .name = sym.name});

      if (sym.address != 0 && sym.address < min_import_addr) {
        min_import_addr = sym.address;
      }
    }
  }
  if (min_import_addr != std::numeric_limits<u32>::max()) {
    view.import_thunk_table_start_ = min_import_addr;
    REXCG_DEBUG("BinaryView: import thunk table starts at 0x{:08X}", min_import_addr);

    for (const auto& section : module.GetBinarySections()) {
      u32 section_end = section.virtual_address + section.virtual_size;
      if (min_import_addr >= section.virtual_address && min_import_addr < section_end) {
        view.import_export_range_end_ = section_end;
        REXCG_DEBUG("BinaryView: import/export range ends at 0x{:08X} (end of {} section)",
                    view.import_export_range_end_, section.name);
        break;
      }
    }
  }
  REXCG_DEBUG("BinaryView: copied {} import symbols", view.import_symbols_.size());

  const auto& binary_sections = module.GetBinarySections();
  view.section_names_.reserve(binary_sections.size());
  view.section_data_.reserve(binary_sections.size());
  view.sections_.reserve(binary_sections.size());

  u32 image_end = view.base_address_ + view.image_size_;
  for (const auto& section : binary_sections) {
    if (!section.host_data || section.virtual_size == 0) {
      REXCG_DEBUG("BinaryView: skipping unmapped section '{}'", section.name);
      continue;
    }
    if (section.virtual_address + section.virtual_size > image_end) {
      REXCG_DEBUG("BinaryView: skipping section '{}' (extends past image end 0x{:08X})",
                  section.name, image_end);
      continue;
    }

    view.section_names_.push_back(std::string(section.name));

    view.section_data_.emplace_back(section.host_data, section.host_data + section.virtual_size);

    view.sections_.push_back(SectionView{.name = view.section_names_.back(),
                                         .base_address = section.virtual_address,
                                         .size = section.virtual_size,
                                         .data = view.section_data_.back().data(),
                                         .executable = section.executable});

    REXCG_DEBUG("BinaryView: section '{}' at 0x{:08X} size 0x{:X} exec={}", section.name,
                section.virtual_address, section.virtual_size, section.executable);
  }

  REXCG_DEBUG("BinaryView: loaded {} sections, base=0x{:08X}, size=0x{:X}", view.sections_.size(),
              view.base_address_, view.image_size_);

  return view;
}

const u8* BinaryView::Translate(u32 addr) const {
  for (const auto& section : sections_) {
    if (auto* ptr = section.Translate(addr)) {
      return ptr;
    }
  }
  return nullptr;
}

bool BinaryView::IsExecutable(u32 addr) const {
  if (auto* section = FindSection(addr)) {
    return section->executable;
  }
  return false;
}

const SectionView* BinaryView::FindSection(u32 addr) const {
  for (const auto& section : sections_) {
    if (section.Contains(addr)) {
      return &section;
    }
  }
  return nullptr;
}

const SectionView* BinaryView::FindSectionByName(std::string_view name) const {
  for (const auto& section : sections_) {
    if (section.name == name) {
      return &section;
    }
  }
  return nullptr;
}

}  // namespace rex::codegen
