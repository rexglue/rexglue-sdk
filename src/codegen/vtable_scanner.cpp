/**
 * @file        rexcodegen/vtable_scanner.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/codegen/vtable_scanner.h>
#include <rex/logging.h>

#include "codegen_logging.h"
#include "ppc/instruction.h"
#include <rex/memory/utils.h>
#include <rex/types.h>

using rex::memory::load_and_swap;
using rex::codegen::ppc::decode_instruction;

namespace rex::codegen {

VTableScanner::VTableScanner(const BinaryView& binary) : binary_(binary) {}

std::vector<VTableInfo> VTableScanner::scan() {
  std::vector<VTableInfo> vtables;

  // Step 1: Find all Complete Object Locators
  auto cols = findCompleteObjectLocators();
  REXCODEGEN_DEBUG("VTableScanner: found {} Complete Object Locators", cols.size());

  // Step 2: For each COL, find its vtable and read slots
  for (uint32_t colAddr : cols) {
    auto vtableAddr = findVTableForCOL(colAddr);
    if (!vtableAddr) {
      REXCODEGEN_TRACE("VTableScanner: COL at 0x{:08X} has no referencing vtable", colAddr);
      continue;
    }

    VTableInfo info;
    info.vtableAddress = *vtableAddr;
    info.colAddress = colAddr;
    info.className = extractClassName(colAddr);
    info.slots = readVTableSlots(*vtableAddr);

    if (info.slots.empty()) {
      REXCODEGEN_TRACE("VTableScanner: vtable at 0x{:08X} has no valid slots", *vtableAddr);
      continue;
    }

    REXCODEGEN_DEBUG("VTableScanner: vtable at 0x{:08X} ({}) has {} slots", info.vtableAddress,
                     info.className, info.slots.size());

    vtables.push_back(std::move(info));
  }

  // Step 3: Fallback scan for raw function pointer tables in non-executable sections.
  // Many Xbox 360 titles ship callback/vtable-style tables without MSVC RTTI, and we still
  // want their targets promoted into real functions for indirect dispatch.
  auto pointerTables = scanPointerTables();
  if (!pointerTables.empty()) {
    REXCODEGEN_DEBUG("VTableScanner: found {} raw pointer tables without RTTI",
                     pointerTables.size());
  }

  for (auto& table : pointerTables) {
    bool duplicate = false;
    for (const auto& existing : vtables) {
      if (existing.vtableAddress == table.vtableAddress) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate) {
      vtables.push_back(std::move(table));
    }
  }

  return vtables;
}

std::vector<VTableInfo> VTableScanner::scanPointerTables() {
  std::vector<VTableInfo> tables;

  // Runs of 1 are accepted: games embed single function pointers in data
  // structs (e.g. a lone callback slot) and those targets must still become
  // functions for indirect dispatch. Every slot is individually gated by the
  // alignment/executability/boundary checks below, so the run length adds no
  // safety of its own.
  constexpr size_t kMinPointerRun = 1;

  for (const auto& section : binary_.sections()) {
    if (section.executable || !section.data || section.size < kMinPointerRun * 4) {
      continue;
    }

    size_t offset = 0;
    while (offset + kMinPointerRun * 4 <= section.size) {
      std::vector<uint32_t> slots;
      size_t scan_offset = offset;

      while (scan_offset + 4 <= section.size) {
        uint32_t value = load_and_swap<uint32_t>(section.data + scan_offset);
        if (value == 0 || (value & 0x3) != 0 || !isExecutableAddress(value) ||
            binary_.isInImportExportRange(value) || !isLikelyFunctionEntry(value)) {
          break;
        }

        slots.push_back(value);
        scan_offset += 4;
      }

      if (slots.size() >= kMinPointerRun) {
        uint32_t tableAddr = section.baseAddress + static_cast<uint32_t>(offset);
        REXCODEGEN_TRACE("VTableScanner: raw pointer table at 0x{:08X} in {} has {} slots",
                         tableAddr, section.name, slots.size());
        tables.push_back(VTableInfo{
            .vtableAddress = tableAddr,
            .colAddress = 0,
            .className = std::string{},
            .slots = std::move(slots),
        });
        offset = scan_offset;
        continue;
      }

      offset += 4;
    }
  }

  return tables;
}

std::vector<uint32_t> VTableScanner::findCompleteObjectLocators() {
  std::vector<uint32_t> cols;

  // Scan .rdata section for COL patterns
  const auto* rdata = binary_.findSectionByName(".rdata");
  if (!rdata || !rdata->data) {
    REXCODEGEN_WARN("VTableScanner: .rdata section not found");
    return cols;
  }

  const uint8_t* data = rdata->data;
  uint32_t base = rdata->baseAddress;
  size_t size = rdata->size;

  // COL is 20 bytes, need room for it
  if (size < sizeof(RTTICompleteObjectLocator)) {
    return cols;
  }

  // Scan for COL pattern: signature=0, valid type descriptor pointer
  for (size_t offset = 0; offset + sizeof(RTTICompleteObjectLocator) <= size; offset += 4) {
    auto* col = reinterpret_cast<const RTTICompleteObjectLocator*>(data + offset);

    uint32_t signature = load_and_swap<uint32_t>(&col->signature);
    uint32_t typeDescPtr = load_and_swap<uint32_t>(&col->pTypeDescriptor);

    // COL signature must be 0 for 32-bit MSVC RTTI
    if (signature != 0) {
      continue;
    }

    // Type descriptor must point to valid memory
    const auto* typeDescSection = binary_.findSection(typeDescPtr);
    if (!typeDescSection || !typeDescSection->data) {
      continue;
    }

    // Check if type descriptor has ".?AV" mangling prefix
    std::string typeName = readString(typeDescPtr + 8, 64);
    if (typeName.find(".?AV") != 0 && typeName.find(".?AU") != 0) {
      continue;
    }

    uint32_t colAddr = base + static_cast<uint32_t>(offset);
    cols.push_back(colAddr);

    REXCODEGEN_TRACE("VTableScanner: found COL at 0x{:08X} -> {}", colAddr, typeName);
  }

  return cols;
}

std::optional<uint32_t> VTableScanner::findVTableForCOL(uint32_t colAddr) {
  // The vtable pointer to COL is stored at vtable[-1]
  // So we need to find a dword in .rdata that contains colAddr,
  // and the vtable starts at that address + 4

  const auto* rdata = binary_.findSectionByName(".rdata");
  if (!rdata || !rdata->data) {
    return std::nullopt;
  }

  const uint8_t* data = rdata->data;
  uint32_t base = rdata->baseAddress;
  size_t size = rdata->size;

  for (size_t offset = 0; offset + 4 <= size; offset += 4) {
    uint32_t value = load_and_swap<uint32_t>(data + offset);

    if (value == colAddr) {
      // Found reference to COL - vtable starts at next dword
      uint32_t vtableAddr = base + static_cast<uint32_t>(offset) + 4;
      return vtableAddr;
    }
  }

  return std::nullopt;
}

std::vector<uint32_t> VTableScanner::readVTableSlots(uint32_t vtableStart) {
  std::vector<uint32_t> slots;

  uint32_t slotAddr = vtableStart;

  while (true) {
    auto funcAddr = readDword(slotAddr);
    if (!funcAddr) {
      break;  // Can't read memory
    }

    uint32_t addr = *funcAddr;

    // Termination: null pointer
    if (addr == 0) {
      break;
    }

    // Termination: not executable address
    if (!isExecutableAddress(addr)) {
      break;
    }

    // Termination: not 4-byte aligned (PPC requirement)
    if (addr & 0x3) {
      break;
    }

    slots.push_back(addr);
    slotAddr += 4;
  }

  return slots;
}

std::string VTableScanner::extractClassName(uint32_t colAddr) {
  auto typeDescPtr = readDword(colAddr + 12);  // pTypeDescriptor offset
  if (!typeDescPtr) {
    return "";
  }

  // Class name is at typeDescriptor + 8
  std::string mangled = readString(*typeDescPtr + 8, 256);

  // Simple demangling: ".?AVClassName@@" -> "ClassName"
  if (mangled.size() > 4 && (mangled.substr(0, 4) == ".?AV" || mangled.substr(0, 4) == ".?AU")) {
    size_t end = mangled.find("@@");
    if (end != std::string::npos) {
      return mangled.substr(4, end - 4);
    }
  }

  return mangled;
}

bool VTableScanner::isExecutableAddress(uint32_t addr) const {
  return binary_.isExecutable(addr);
}

bool VTableScanner::isLikelyFunctionEntry(uint32_t addr) const {
  const auto* section = binary_.findSection(addr);
  if (!section || !section->data || !section->executable) {
    return false;
  }

  // Section start is always a valid boundary.
  if (addr == section->baseAddress) {
    return true;
  }

  // If the previous dword isn't in the same executable section, treat this as a boundary too.
  if (addr < section->baseAddress + 4) {
    return true;
  }

  uint32_t prevAddr = addr - 4;
  if (!section->contains(prevAddr)) {
    return true;
  }

  uint32_t prevOffset = prevAddr - section->baseAddress;
  if (prevOffset + 4 > section->size) {
    return true;
  }

  uint32_t prevRaw = load_and_swap<uint32_t>(section->data + prevOffset);
  auto prevInsn = decode_instruction(prevAddr, prevRaw);

  // Accept boundaries after terminating control flow. Reject plain fallthrough into the middle of
  // an existing block, which is how internal labels like 0x824D2920 usually appear.
  if (prevInsn.is_return() || prevInsn.is_indirect_branch()) {
    return true;
  }
  if (prevInsn.is_branch() && !prevInsn.is_call()) {
    return true;
  }

  return false;
}

std::optional<uint32_t> VTableScanner::readDword(uint32_t addr) const {
  const auto* section = binary_.findSection(addr);
  if (!section || !section->data) {
    return std::nullopt;
  }

  uint32_t offset = addr - section->baseAddress;
  if (offset + 4 > section->size) {
    return std::nullopt;
  }

  return load_and_swap<uint32_t>(section->data + offset);
}

std::string VTableScanner::readString(uint32_t addr, size_t maxLen) const {
  const auto* section = binary_.findSection(addr);
  if (!section || !section->data) {
    return "";
  }

  uint32_t offset = addr - section->baseAddress;
  size_t available = section->size - offset;
  size_t len = std::min(maxLen, available);

  const char* str = reinterpret_cast<const char*>(section->data + offset);
  size_t actualLen = strnlen(str, len);

  return std::string(str, actualLen);
}

}  // namespace rex::codegen
