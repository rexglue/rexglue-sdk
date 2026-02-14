/**
 * @file        rex/codegen/vtable_scanner.h
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

#include <rex/codegen/binary_view.h>

namespace rex::codegen {

//=============================================================================
// RTTI Structures (MSVC-based, Xbox 360)
//=============================================================================

// Type descriptor - contains mangled class name
struct RTTITypeDescriptor {
  uint32_t pVFTable;  // Always points to type_info vtable
  uint32_t spare;     // Runtime use, always 0 in image
                      // char name[];         // Mangled name: ".?AVClassName@@"
};

// Complete Object Locator - links vtable to type info
struct RTTICompleteObjectLocator {
  uint32_t signature;        // Always 0 for 32-bit
  uint32_t offset;           // Offset of this vtable in complete class
  uint32_t cdOffset;         // Constructor displacement offset
  uint32_t pTypeDescriptor;  // Pointer to TypeDescriptor
  uint32_t pClassHierarchy;  // Pointer to class hierarchy descriptor
};

//=============================================================================
// VTableInfo - Result of vtable scanning
//=============================================================================

struct VTableInfo {
  uint32_t vtableAddress;       // Address of vtable[0]
  uint32_t colAddress;          // RTTI Complete Object Locator address
  std::string className;        // Demangled class name (optional)
  std::vector<uint32_t> slots;  // Function addresses in vtable
};

//=============================================================================
// VTableScanner - RTTI-based vtable discovery
//=============================================================================

class VTableScanner {
 public:
  explicit VTableScanner(const BinaryView& binary);

  // Scan for all vtables via RTTI traversal
  std::vector<VTableInfo> scan();

 private:
  const BinaryView& binary_;

  // Find all Complete Object Locators in .rdata
  std::vector<uint32_t> findCompleteObjectLocators();

  // Find the vtable that references a given COL (COL is at vtable[-1])
  std::optional<uint32_t> findVTableForCOL(uint32_t colAddr);

  // Read all function slots from a vtable (until non-executable address)
  std::vector<uint32_t> readVTableSlots(uint32_t vtableStart);

  // Extract class name from type descriptor
  std::string extractClassName(uint32_t colAddr);

  // Check if an address points to executable code
  bool isExecutableAddress(uint32_t addr) const;

  // Read a dword from the binary
  std::optional<uint32_t> readDword(uint32_t addr) const;

  // Read a string from the binary
  std::string readString(uint32_t addr, size_t maxLen = 256) const;
};

}  // namespace rex::codegen
