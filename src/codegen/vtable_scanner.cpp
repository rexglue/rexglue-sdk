#include "vtable_scanner.h"
#include <rex/logging/log.h>
#include "codegen_categories.h"

#include <rex/memory/endian.h>
#include <rex/types.h>

namespace rex::codegen {

VTableScanner::VTableScanner(const BinaryView& binary) : binary_(binary) {}

std::vector<VTable> VTableScanner::Scan() {
  std::vector<VTable> vtables;

  auto cols = FindCompleteObjectLocators();
  REXCG_DEBUG("VTableScanner: found {} Complete Object Locators", cols.size());

  for (u32 col_addr : cols) {
    auto vtable_addr = FindVTableForCOL(col_addr);
    if (!vtable_addr) {
      REXCG_TRACE("VTableScanner: COL at 0x{:08X} has no referencing vtable", col_addr);
      continue;
    }

    VTable info;
    info.vtable_address = *vtable_addr;
    info.col_address = col_addr;
    info.class_name = ExtractClassName(col_addr);
    info.slots = ReadVTableSlots(*vtable_addr);

    if (info.slots.empty()) {
      REXCG_TRACE("VTableScanner: vtable at 0x{:08X} has no valid slots", *vtable_addr);
      continue;
    }

    REXCG_DEBUG("VTableScanner: vtable at 0x{:08X} ({}) has {} slots", info.vtable_address,
                info.class_name, info.slots.size());

    vtables.push_back(std::move(info));
  }

  return vtables;
}

/** Finds the Complete Object Locators in .rdata. */
std::vector<u32> VTableScanner::FindCompleteObjectLocators() {
  std::vector<u32> cols;

  const auto* rdata = binary_.FindSectionByName(".rdata");
  if (!rdata || !rdata->data) {
    REXCG_WARN("VTableScanner: .rdata section not found");
    return cols;
  }

  const u8* data = rdata->data;
  u32 base = rdata->base_address;
  size_t size = rdata->size;

  if (size < sizeof(RTTICompleteObjectLocator)) {
    return cols;
  }

  for (size_t offset = 0; offset + sizeof(RTTICompleteObjectLocator) <= size; offset += 4) {
    auto* col = reinterpret_cast<const RTTICompleteObjectLocator*>(data + offset);

    u32 signature = col->signature;
    u32 type_desc_ptr = col->pTypeDescriptor;

    if (signature != 0) {
      continue;
    }

    const auto* type_desc_section = binary_.FindSection(type_desc_ptr);
    if (!type_desc_section || !type_desc_section->data) {
      continue;
    }

    std::string type_name = ReadString(type_desc_ptr + 8, 64);
    if (type_name.find(".?AV") != 0 && type_name.find(".?AU") != 0) {
      continue;
    }

    u32 col_addr = base + static_cast<u32>(offset);
    cols.push_back(col_addr);

    REXCG_TRACE("VTableScanner: found COL at 0x{:08X} -> {}", col_addr, type_name);
  }

  return cols;
}

/**
 * Finds the vtable of a locator. The locator pointer sits at vtable[-1], so the vtable starts 1
 * dword after the .rdata word that holds the locator address.
 *
 * @return The vtable address, or nullopt when nothing references the locator.
 */
std::optional<u32> VTableScanner::FindVTableForCOL(u32 col_addr) {
  const auto* rdata = binary_.FindSectionByName(".rdata");
  if (!rdata || !rdata->data) {
    return std::nullopt;
  }

  const u8* data = rdata->data;
  u32 base = rdata->base_address;
  size_t size = rdata->size;

  for (size_t offset = 0; offset + 4 <= size; offset += 4) {
    u32 value = memory::load<be_u32>(data + offset);

    if (value == col_addr) {
      u32 vtable_addr = base + static_cast<u32>(offset) + 4;
      return vtable_addr;
    }
  }

  return std::nullopt;
}

/**
 * Reads vtable slots until a null, non executable or non 4 byte aligned pointer.
 *
 * @param vtable_start Address of slot 0.
 * @return The slot function addresses.
 */
std::vector<u32> VTableScanner::ReadVTableSlots(u32 vtable_start) {
  std::vector<u32> slots;

  u32 slot_addr = vtable_start;

  while (true) {
    auto func_addr = ReadWord(slot_addr);
    if (!func_addr) {
      break;
    }

    u32 addr = *func_addr;

    if (addr == 0) {
      break;
    }

    if (!IsExecutableAddress(addr)) {
      break;
    }

    if (addr & 0x3) {
      break;
    }

    slots.push_back(addr);
    slot_addr += 4;
  }

  return slots;
}

/**
 * Reads the class name from the type descriptor at locator offset 12, name at +8, and strips the
 * ".?AV" or ".?AU" prefix and the "@@" suffix.
 *
 * @return The class name, or the mangled text when it has no known prefix.
 */
std::string VTableScanner::ExtractClassName(u32 col_addr) {
  auto type_desc_ptr = ReadWord(col_addr + 12);
  if (!type_desc_ptr) {
    return "";
  }

  std::string mangled = ReadString(*type_desc_ptr + 8, 256);

  if (mangled.size() > 4 && (mangled.substr(0, 4) == ".?AV" || mangled.substr(0, 4) == ".?AU")) {
    size_t end = mangled.find("@@");
    if (end != std::string::npos) {
      return mangled.substr(4, end - 4);
    }
  }

  return mangled;
}

bool VTableScanner::IsExecutableAddress(u32 addr) const {
  return binary_.IsExecutable(addr);
}

std::optional<u32> VTableScanner::ReadWord(u32 addr) const {
  const auto* section = binary_.FindSection(addr);
  if (!section || !section->data) {
    return std::nullopt;
  }

  u32 offset = addr - section->base_address;
  if (offset + 4 > section->size) {
    return std::nullopt;
  }

  return memory::load<be_u32>(section->data + offset);
}

std::string VTableScanner::ReadString(u32 addr, size_t max_len) const {
  const auto* section = binary_.FindSection(addr);
  if (!section || !section->data) {
    return "";
  }

  u32 offset = addr - section->base_address;
  size_t available = section->size - offset;
  size_t len = std::min(max_len, available);

  const char* str = reinterpret_cast<const char*>(section->data + offset);
  size_t actual_len = strnlen(str, len);

  return std::string(str, actual_len);
}

}  // namespace rex::codegen
