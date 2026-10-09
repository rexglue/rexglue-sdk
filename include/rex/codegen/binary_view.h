/**
 * @file        codegen/binary_view.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/types.h>

namespace rex::runtime {
class Module;
}

namespace rex::codegen {

/** Lightweight view of a binary section that points into the BinaryView owned data. */
struct SectionView {
  std::string_view name; /**< Section name from the image, such as `.text`. */
  uint32_t baseAddress;  /**< Guest address of the first byte of the section. */
  uint32_t size;         /**< Section size in bytes. */
  const uint8_t* data;   /**< Host pointer to the section bytes, `size` bytes long. */
  /** True for sections the image marks executable, which the scanners treat as code. */
  bool executable;

  bool contains(uint32_t addr) const { return addr >= baseAddress && addr < baseAddress + size; }

  /**
   * Translates an address to a pointer into this section.
   *
   * @param addr Guest address
   * @return Pointer to the byte at addr, or null when addr is outside the section
   */
  const uint8_t* translate(uint32_t addr) const {
    if (!contains(addr))
      return nullptr;
    return data + (addr - baseAddress);
  }

  uint32_t end() const { return baseAddress + size; }
};

/** Import symbol with its thunk address and name. */
struct ImportSymbol {
  uint32_t address; /**< Thunk address, the bl target */
  std::string name; /**< Name in libname@ordinal format */
};

/** Binary view that owns all section data. */
class BinaryView : public rex::MoveOnly {
 public:
  /**
   * Copies all data from a Module.
   *
   * @param module Source module
   * @return Owning view of the module
   */
  static BinaryView fromModule(const runtime::Module& module);

  /**
   * Translates an address to a pointer into the owned section data.
   *
   * @param addr Guest address
   * @return Pointer to the byte at addr, or null when no section contains addr
   */
  const uint8_t* translate(uint32_t addr) const;
  /**
   * Reports whether an address is in an executable section.
   *
   * @param addr Guest address
   * @return True when a section contains addr and is executable
   */
  bool isExecutable(uint32_t addr) const;
  /**
   * Finds the section containing an address.
   *
   * @param addr Guest address
   * @return The section, or null when none contains addr
   */
  const SectionView* findSection(uint32_t addr) const;
  /**
   * Finds a section by name.
   *
   * @param name Section name such as ".text"
   * @return The first section with that name, or null when none matches
   */
  const SectionView* findSectionByName(std::string_view name) const;
  std::span<const SectionView> sections() const { return sections_; }

  uint32_t baseAddress() const { return baseAddress_; }
  uint32_t imageSize() const { return imageSize_; }
  uint32_t entryPoint() const { return entryPoint_; }
  uint32_t exceptionDirectoryAddr() const { return exceptionDirectoryAddr_; }
  uint32_t exceptionDirectorySize() const { return exceptionDirectorySize_; }
  uint32_t exportTableAddr() const { return exportTableAddr_; }

  /**
   * Returns the start of the import thunk table, or 0 when absent.
   * Everything from this address to the end of .text is import and export tables, not code.
   */
  uint32_t importThunkTableStart() const { return importThunkTableStart_; }

  /**
   * Reports whether an address is in the import thunk and export table range, which is not code.
   * The range lies within the .text section only.
   *
   * @param addr Address to test
   * @return True when addr is inside the range
   */
  bool isInImportExportRange(uint32_t addr) const {
    return importThunkTableStart_ != 0 && addr >= importThunkTableStart_ &&
           addr < importExportRangeEnd_;
  }

  /** Returns the import symbols with thunk addresses and names. */
  std::span<const ImportSymbol> importSymbols() const { return importSymbols_; }

 public:
  BinaryView() = default;

 private:
  std::vector<SectionView> sections_;
  std::vector<std::string> sectionNames_;
  std::vector<std::vector<uint8_t>> sectionData_;

  uint32_t baseAddress_ = 0;
  uint32_t imageSize_ = 0;
  uint32_t entryPoint_ = 0;
  uint32_t exceptionDirectoryAddr_ = 0;
  uint32_t exceptionDirectorySize_ = 0;
  uint32_t exportTableAddr_ = 0;
  uint32_t importThunkTableStart_ = 0;
  uint32_t importExportRangeEnd_ = 0;

  std::vector<ImportSymbol> importSymbols_;
};

}  // namespace rex::codegen
