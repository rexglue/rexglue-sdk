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
  u32 baseAddress;       /**< Guest address of the first byte of the section. */
  u32 size;              /**< Section size in bytes. */
  const u8* data;        /**< Host pointer to the section bytes, `size` bytes long. */
  /** True for sections the image marks executable, which the scanners treat as code. */
  bool executable;

  bool contains(u32 addr) const { return addr >= baseAddress && addr < baseAddress + size; }

  /**
   * Translates an address to a pointer into this section.
   *
   * @param addr Guest address
   * @return Pointer to the byte at addr, or null when addr is outside the section
   */
  const u8* translate(u32 addr) const {
    if (!contains(addr))
      return nullptr;
    return data + (addr - baseAddress);
  }

  u32 end() const { return baseAddress + size; }
};

/** Import symbol with its thunk address and name. */
struct ImportSymbol {
  u32 address;      /**< Thunk address, the bl target */
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
  const u8* translate(u32 addr) const;
  /**
   * Reports whether an address is in an executable section.
   *
   * @param addr Guest address
   * @return True when a section contains addr and is executable
   */
  bool isExecutable(u32 addr) const;
  /**
   * Finds the section containing an address.
   *
   * @param addr Guest address
   * @return The section, or null when none contains addr
   */
  const SectionView* findSection(u32 addr) const;
  /**
   * Finds a section by name.
   *
   * @param name Section name such as ".text"
   * @return The first section with that name, or null when none matches
   */
  const SectionView* findSectionByName(std::string_view name) const;
  std::span<const SectionView> sections() const { return sections_; }

  u32 baseAddress() const { return baseAddress_; }
  u32 imageSize() const { return imageSize_; }
  u32 entryPoint() const { return entryPoint_; }
  u32 exceptionDirectoryAddr() const { return exceptionDirectoryAddr_; }
  u32 exceptionDirectorySize() const { return exceptionDirectorySize_; }
  u32 exportTableAddr() const { return exportTableAddr_; }

  /**
   * Returns the start of the import thunk table, or 0 when absent.
   * Everything from this address to the end of .text is import and export tables, not code.
   */
  u32 importThunkTableStart() const { return importThunkTableStart_; }

  /**
   * Reports whether an address is in the import thunk and export table range, which is not code.
   * The range lies within the .text section only.
   *
   * @param addr Address to test
   * @return True when addr is inside the range
   */
  bool isInImportExportRange(u32 addr) const {
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
  std::vector<std::vector<u8>> sectionData_;

  u32 baseAddress_ = 0;
  u32 imageSize_ = 0;
  u32 entryPoint_ = 0;
  u32 exceptionDirectoryAddr_ = 0;
  u32 exceptionDirectorySize_ = 0;
  u32 exportTableAddr_ = 0;
  u32 importThunkTableStart_ = 0;
  u32 importExportRangeEnd_ = 0;

  std::vector<ImportSymbol> importSymbols_;
};

}  // namespace rex::codegen
