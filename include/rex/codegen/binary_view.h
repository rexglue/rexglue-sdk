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
  u32 base_address;      /**< Guest address of the first byte of the section. */
  u32 size;              /**< Section size in bytes. */
  const u8* data;        /**< Host pointer to the section bytes, `size` bytes long. */
  /** True for sections the image marks executable, which the scanners treat as code. */
  bool executable;

  /**
   * Reports whether an address is inside the section.
   *
   * @param addr Guest address to test.
   * @return True when addr is within the section.
   */
  bool Contains(u32 addr) const { return addr >= base_address && addr < base_address + size; }

  /**
   * Translates an address to a pointer into this section.
   *
   * @param addr Guest address
   * @return Pointer to the byte at addr, or null when addr is outside the section
   */
  const u8* Translate(u32 addr) const {
    if (!Contains(addr))
      return nullptr;
    return data + (addr - base_address);
  }

  /** Returns the guest address 1 past the section. */
  u32 GetEnd() const { return base_address + size; }
};

/** Import symbol with its thunk address and name. */
struct ImportSymbol {
  u32 address;      /**< Thunk address, the bl target */
  std::string name; /**< Name in libname\@ordinal format */
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
  static BinaryView FromModule(const runtime::Module& module);

  /**
   * Translates an address to a pointer into the owned section data.
   *
   * @param addr Guest address
   * @return Pointer to the byte at addr, or null when no section contains addr
   */
  const u8* Translate(u32 addr) const;
  /**
   * Reports whether an address is in an executable section.
   *
   * @param addr Guest address
   * @return True when a section contains addr and is executable
   */
  bool IsExecutable(u32 addr) const;
  /**
   * Finds the section containing an address.
   *
   * @param addr Guest address
   * @return The section, or null when none contains addr
   */
  const SectionView* FindSection(u32 addr) const;
  /**
   * Finds a section by name.
   *
   * @param name Section name such as ".text"
   * @return The first section with that name, or null when none matches
   */
  const SectionView* FindSectionByName(std::string_view name) const;
  /** Returns all sections of the image. */
  std::span<const SectionView> GetSections() const { return sections_; }

  /** Returns the guest address the image is loaded at. */
  u32 GetBaseAddress() const { return base_address_; }
  /** Returns the image size in bytes. */
  u32 GetImageSize() const { return image_size_; }
  /** Returns the guest address of the image entry point. */
  u32 GetEntryPoint() const { return entry_point_; }
  /** Returns the guest address of the exception directory. */
  u32 GetExceptionDirectoryAddress() const { return exception_directory_addr_; }
  /** Returns the size of the exception directory in bytes. */
  u32 GetExceptionDirectorySize() const { return exception_directory_size_; }
  /** Returns the guest address of the export table. */
  u32 GetExportTableAddress() const { return export_table_addr_; }

  /**
   * Returns the start of the import thunk table, or 0 when absent.
   * Everything from this address to the end of .text is import and export tables, not code.
   */
  u32 GetImportThunkTableStart() const { return import_thunk_table_start_; }

  /**
   * Reports whether an address is in the import thunk and export table range, which is not code.
   * The range lies within the .text section only.
   *
   * @param addr Address to test
   * @return True when addr is inside the range
   */
  bool IsInImportExportRange(u32 addr) const {
    return import_thunk_table_start_ != 0 && addr >= import_thunk_table_start_ &&
           addr < import_export_range_end_;
  }

  /** Returns the import symbols with thunk addresses and names. */
  std::span<const ImportSymbol> GetImportSymbols() const { return import_symbols_; }

  BinaryView() = default;

 private:
  std::vector<SectionView> sections_;
  std::vector<std::string> section_names_;
  std::vector<std::vector<u8>> section_data_;

  u32 base_address_ = 0;
  u32 image_size_ = 0;
  u32 entry_point_ = 0;
  u32 exception_directory_addr_ = 0;
  u32 exception_directory_size_ = 0;
  u32 export_table_addr_ = 0;
  u32 import_thunk_table_start_ = 0;
  u32 import_export_range_end_ = 0;

  std::vector<ImportSymbol> import_symbols_;
};

}  // namespace rex::codegen
