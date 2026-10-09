#include <limits>

#include <rex/codegen/binary_view.h>
#include <rex/logging.h>

#include <rex/runtime/binary_types.h>
#include <rex/runtime/module.h>

namespace rex::codegen {

BinaryView BinaryView::fromModule(const runtime::Module& module) {
  BinaryView view;

  view.baseAddress_ = module.base_address();
  view.imageSize_ = module.image_size();
  view.entryPoint_ = module.entry_point();
  view.exceptionDirectoryAddr_ = module.exception_directory_address();
  view.exceptionDirectorySize_ = module.exception_directory_size();
  view.exportTableAddr_ = module.export_table_address();

  // Import thunk table and the export table after it extend to the end of .text
  uint32_t minImportAddr = std::numeric_limits<uint32_t>::max();
  for (const auto& sym : module.binary_symbols()) {
    if (sym.type == runtime::BinarySymbolType::Import) {
      view.importSymbols_.push_back(ImportSymbol{.address = sym.address, .name = sym.name});

      if (sym.address != 0 && sym.address < minImportAddr) {
        minImportAddr = sym.address;
      }
    }
  }
  if (minImportAddr != std::numeric_limits<uint32_t>::max()) {
    view.importThunkTableStart_ = minImportAddr;
    REXLOG_DEBUG(codegen, "BinaryView: import thunk table starts at 0x{:08X}", minImportAddr);

    for (const auto& section : module.binary_sections()) {
      uint32_t sectionEnd = section.virtual_address + section.virtual_size;
      if (minImportAddr >= section.virtual_address && minImportAddr < sectionEnd) {
        view.importExportRangeEnd_ = sectionEnd;
        REXLOG_DEBUG(codegen,
                     "BinaryView: import/export range ends at 0x{:08X} (end of {} section)",
                     view.importExportRangeEnd_, section.name);
        break;
      }
    }
  }
  REXLOG_DEBUG(codegen, "BinaryView: copied {} import symbols", view.importSymbols_.size());

  const auto& binarySections = module.binary_sections();
  view.sectionNames_.reserve(binarySections.size());
  view.sectionData_.reserve(binarySections.size());
  view.sections_.reserve(binarySections.size());

  uint32_t imageEnd = view.baseAddress_ + view.imageSize_;
  for (const auto& section : binarySections) {
    if (!section.host_data || section.virtual_size == 0) {
      REXLOG_DEBUG(codegen, "BinaryView: skipping unmapped section '{}'", section.name);
      continue;
    }
    if (section.virtual_address + section.virtual_size > imageEnd) {
      REXLOG_DEBUG(codegen, "BinaryView: skipping section '{}' (extends past image end 0x{:08X})",
                   section.name, imageEnd);
      continue;
    }

    view.sectionNames_.push_back(std::string(section.name));

    view.sectionData_.emplace_back(section.host_data, section.host_data + section.virtual_size);

    view.sections_.push_back(SectionView{.name = view.sectionNames_.back(),
                                         .baseAddress = section.virtual_address,
                                         .size = section.virtual_size,
                                         .data = view.sectionData_.back().data(),
                                         .executable = section.executable});

    REXLOG_DEBUG(codegen, "BinaryView: section '{}' at 0x{:08X} size 0x{:X} exec={}", section.name,
                 section.virtual_address, section.virtual_size, section.executable);
  }

  REXLOG_DEBUG(codegen, "BinaryView: loaded {} sections, base=0x{:08X}, size=0x{:X}",
               view.sections_.size(), view.baseAddress_, view.imageSize_);

  return view;
}

const uint8_t* BinaryView::translate(uint32_t addr) const {
  for (const auto& section : sections_) {
    if (auto* ptr = section.translate(addr)) {
      return ptr;
    }
  }
  return nullptr;
}

bool BinaryView::isExecutable(uint32_t addr) const {
  if (auto* section = findSection(addr)) {
    return section->executable;
  }
  return false;
}

const SectionView* BinaryView::findSection(uint32_t addr) const {
  for (const auto& section : sections_) {
    if (section.contains(addr)) {
      return &section;
    }
  }
  return nullptr;
}

const SectionView* BinaryView::findSectionByName(std::string_view name) const {
  for (const auto& section : sections_) {
    if (section.name == name) {
      return &section;
    }
  }
  return nullptr;
}

}  // namespace rex::codegen
