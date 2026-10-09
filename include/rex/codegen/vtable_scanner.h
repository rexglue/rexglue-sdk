/**
 * @file        codegen/vtable_scanner.h
 * @brief       VTable scanner - RTTI-based vtable discovery
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
#include <rex/codegen/binary_view.h>
#include <rex/types.h>

namespace rex::codegen {

/**
 * MSVC RTTI type descriptor. A char name[] holding the mangled class name such as
 * ".?AVClassName@@" follows the fields.
 */
struct RTTITypeDescriptor {
  be_u32 pVFTable; /**< Always points to the type_info vtable */
  be_u32 spare;    /**< Runtime use, always 0 in the image */
};

/** MSVC RTTI Complete Object Locator, linking a vtable to its type info. */
struct RTTICompleteObjectLocator {
  be_u32 signature;       /**< Always 0 for 32 bit */
  be_u32 offset;          /**< Offset of this vtable in the complete class */
  be_u32 cdOffset;        /**< Constructor displacement offset */
  be_u32 pTypeDescriptor; /**< Offset 0x0C. Guest address of the RTTITypeDescriptor of the class. */
  be_u32 pClassHierarchy; /**< Pointer to the class hierarchy descriptor */
};
REX_STATIC_ASSERT_SIZE(RTTICompleteObjectLocator, 20);

/** Result of vtable scanning. */
struct VTableInfo {
  uint32_t vtableAddress;      /**< Address of vtable[0] */
  uint32_t colAddress;         /**< RTTI Complete Object Locator address */
  std::string className;       /**< Demangled class name, optional */
  std::vector<uint32_t> slots; /**< Function addresses in the vtable */
};

/** RTTI based vtable discovery. */
class VTableScanner {
 public:
  /**
   * Creates a scanner over a binary.
   *
   * @param binary Binary to scan, must outlive the scanner
   */
  explicit VTableScanner(const BinaryView& binary);

  /**
   * Scans for all vtables through RTTI traversal.
   *
   * @return Vtables found
   */
  std::vector<VTableInfo> scan();

 private:
  const BinaryView& binary_;

  std::vector<uint32_t> findCompleteObjectLocators();

  /**
   * Finds the vtable that references a COL, which sits at vtable[-1].
   *
   * @param colAddr COL address
   * @return Vtable address, or nullopt when none references it
   */
  std::optional<uint32_t> findVTableForCOL(uint32_t colAddr);

  /**
   * Reads function slots from a vtable until a non executable address.
   *
   * @param vtableStart Address of vtable[0]
   * @return Slot function addresses
   */
  std::vector<uint32_t> readVTableSlots(uint32_t vtableStart);

  std::string extractClassName(uint32_t colAddr);

  bool isExecutableAddress(uint32_t addr) const;

  std::optional<uint32_t> readDword(uint32_t addr) const;

  std::string readString(uint32_t addr, size_t maxLen = 256) const;
};

}  // namespace rex::codegen
