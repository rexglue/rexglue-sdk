#include <rex/codegen/vtable_scanner.h>
#include <rex/logging.h>

#include <rex/memory/utils.h>
#include <rex/types.h>

namespace rex::codegen {

VTableScanner::VTableScanner(const BinaryView& binary) : binary_(binary) {}

std::vector<VTableInfo> VTableScanner::scan() {
  std::vector<VTableInfo> vtables;

  auto cols = findCompleteObjectLocators();
  REXLOG_DEBUG(codegen, "VTableScanner: found {} Complete Object Locators", cols.size());

  for (u32 colAddr : cols) {
    auto vtableAddr = findVTableForCOL(colAddr);
    if (!vtableAddr) {
      REXLOG_TRACE(codegen, "VTableScanner: COL at 0x{:08X} has no referencing vtable", colAddr);
      continue;
    }

    VTableInfo info;
    info.vtableAddress = *vtableAddr;
    info.colAddress = colAddr;
    info.className = extractClassName(colAddr);
    info.slots = readVTableSlots(*vtableAddr);

    if (info.slots.empty()) {
      REXLOG_TRACE(codegen, "VTableScanner: vtable at 0x{:08X} has no valid slots", *vtableAddr);
      continue;
    }

    REXLOG_DEBUG(codegen, "VTableScanner: vtable at 0x{:08X} ({}) has {} slots", info.vtableAddress,
                 info.className, info.slots.size());

    vtables.push_back(std::move(info));
  }

  return vtables;
}

/**
 * Finds Complete Object Locators in .rdata. A locator is 20 bytes with signature 0 for 32 bit
 * MSVC RTTI and a type descriptor pointer whose name at +8 starts with ".?AV" or ".?AU".
 *
 * @return Addresses of the locators.
 */
std::vector<u32> VTableScanner::findCompleteObjectLocators() {
  std::vector<u32> cols;

  const auto* rdata = binary_.findSectionByName(".rdata");
  if (!rdata || !rdata->data) {
    REXLOG_WARN(codegen, "VTableScanner: .rdata section not found");
    return cols;
  }

  const u8* data = rdata->data;
  u32 base = rdata->baseAddress;
  size_t size = rdata->size;

  if (size < sizeof(RTTICompleteObjectLocator)) {
    return cols;
  }

  for (size_t offset = 0; offset + sizeof(RTTICompleteObjectLocator) <= size; offset += 4) {
    auto* col = reinterpret_cast<const RTTICompleteObjectLocator*>(data + offset);

    u32 signature = col->signature;
    u32 typeDescPtr = col->pTypeDescriptor;

    if (signature != 0) {
      continue;
    }

    const auto* typeDescSection = binary_.findSection(typeDescPtr);
    if (!typeDescSection || !typeDescSection->data) {
      continue;
    }

    std::string typeName = readString(typeDescPtr + 8, 64);
    if (typeName.find(".?AV") != 0 && typeName.find(".?AU") != 0) {
      continue;
    }

    u32 colAddr = base + static_cast<u32>(offset);
    cols.push_back(colAddr);

    REXLOG_TRACE(codegen, "VTableScanner: found COL at 0x{:08X} -> {}", colAddr, typeName);
  }

  return cols;
}

/**
 * Finds the vtable of a locator. The locator pointer sits at vtable[-1], so the vtable starts 1
 * dword after the .rdata word that holds the locator address.
 *
 * @param colAddr Locator address.
 * @return The vtable address, or nullopt when nothing references the locator.
 */
std::optional<u32> VTableScanner::findVTableForCOL(u32 colAddr) {
  const auto* rdata = binary_.findSectionByName(".rdata");
  if (!rdata || !rdata->data) {
    return std::nullopt;
  }

  const u8* data = rdata->data;
  u32 base = rdata->baseAddress;
  size_t size = rdata->size;

  for (size_t offset = 0; offset + 4 <= size; offset += 4) {
    u32 value = memory::load<be_u32>(data + offset);

    if (value == colAddr) {
      u32 vtableAddr = base + static_cast<u32>(offset) + 4;
      return vtableAddr;
    }
  }

  return std::nullopt;
}

/**
 * Reads vtable slots until a null, non executable or non 4 byte aligned pointer.
 *
 * @param vtableStart Address of slot 0.
 * @return The slot function addresses.
 */
std::vector<u32> VTableScanner::readVTableSlots(u32 vtableStart) {
  std::vector<u32> slots;

  u32 slotAddr = vtableStart;

  while (true) {
    auto funcAddr = readDword(slotAddr);
    if (!funcAddr) {
      break;
    }

    u32 addr = *funcAddr;

    if (addr == 0) {
      break;
    }

    if (!isExecutableAddress(addr)) {
      break;
    }

    if (addr & 0x3) {
      break;
    }

    slots.push_back(addr);
    slotAddr += 4;
  }

  return slots;
}

/**
 * Reads the class name from the type descriptor at locator offset 12, name at +8, and strips the
 * ".?AV" or ".?AU" prefix and the "@@" suffix.
 *
 * @param colAddr Locator address.
 * @return The class name, or the mangled text when it has no known prefix.
 */
std::string VTableScanner::extractClassName(u32 colAddr) {
  auto typeDescPtr = readDword(colAddr + 12);
  if (!typeDescPtr) {
    return "";
  }

  std::string mangled = readString(*typeDescPtr + 8, 256);

  if (mangled.size() > 4 && (mangled.substr(0, 4) == ".?AV" || mangled.substr(0, 4) == ".?AU")) {
    size_t end = mangled.find("@@");
    if (end != std::string::npos) {
      return mangled.substr(4, end - 4);
    }
  }

  return mangled;
}

bool VTableScanner::isExecutableAddress(u32 addr) const {
  return binary_.isExecutable(addr);
}

std::optional<u32> VTableScanner::readDword(u32 addr) const {
  const auto* section = binary_.findSection(addr);
  if (!section || !section->data) {
    return std::nullopt;
  }

  u32 offset = addr - section->baseAddress;
  if (offset + 4 > section->size) {
    return std::nullopt;
  }

  return memory::load<be_u32>(section->data + offset);
}

std::string VTableScanner::readString(u32 addr, size_t maxLen) const {
  const auto* section = binary_.findSection(addr);
  if (!section || !section->data) {
    return "";
  }

  u32 offset = addr - section->baseAddress;
  size_t available = section->size - offset;
  size_t len = std::min(maxLen, available);

  const char* str = reinterpret_cast<const char*>(section->data + offset);
  size_t actualLen = strnlen(str, len);

  return std::string(str, actualLen);
}

}  // namespace rex::codegen
