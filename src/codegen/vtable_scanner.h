/**
 * @file        codegen/vtable_scanner.h
 * @brief       VTable scanner using RTTI based vtable discovery
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <rex/assert.h>
#include "binary_view.h"
#include <rex/types.h>

namespace rex::codegen {

/**
 * MSVC RTTI type descriptor. A char name[] holding the mangled class name such as
 * ".?AVClassName@@" follows the fields.
 */
struct RTTITypeDescriptor {
  be_u32 pVFTable; /**< Always points to the type_info vtable. */
  be_u32 spare;
};

/** MSVC RTTI Complete Object Locator, linking a vtable to its type info. */
struct RTTICompleteObjectLocator {
  be_u32 signature; /**< Always 0 for 32 bit. */
  be_u32 offset;    /**< Offset of this vtable in the complete class. */
  be_u32 cd_offset;
  be_u32 pTypeDescriptor; /**< Guest address of the RTTITypeDescriptor of the class. */
  be_u32 pClassHierarchy; /**< Pointer to the class hierarchy descriptor. */
};
REX_STATIC_ASSERT_SIZE(RTTICompleteObjectLocator, 20);

/** Result of vtable scanning. */
struct VTable {
  u32 vtable_address;     /**< Address of vtable[0], after the locator pointer. */
  u32 col_address;        /**< RTTI Complete Object Locator address. */
  std::string class_name; /**< Demangled class name, optional. */
  std::vector<u32> slots; /**< Function addresses in the vtable. */
};

class VTableScanner {
 public:
  /**
   * @param binary Binary to scan, which must outlive the scanner.
   */
  explicit VTableScanner(const BinaryView& binary);

  /** Scans for all vtables through RTTI traversal. */
  std::vector<VTable> Scan();

 private:
  const BinaryView& binary_;

  std::vector<u32> FindCompleteObjectLocators();

  std::optional<u32> FindVTableForCOL(u32 col_addr);

  std::vector<u32> ReadVTableSlots(u32 vtable_start);

  std::string ExtractClassName(u32 col_addr);

  bool IsExecutableAddress(u32 addr) const;

  std::optional<u32> ReadWord(u32 addr) const;

  std::string ReadString(u32 addr, size_t max_len = 256) const;
};

}  // namespace rex::codegen
