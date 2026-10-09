#include <rex/codegen/sig_scanner.h>
#include <rex/logging.h>

#include <rex/memory/utils.h>
#include <rex/types.h>

namespace rex::codegen {

SigScanner::SigScanner(const runtime::Module& module) : module_(module) {}

std::vector<uint32_t> SigScanner::scan(const Signature& sig) {
  std::vector<uint32_t> matches;

  for (const auto& section : module_.binary_sections()) {
    if (!section.host_data)
      continue;

    if (!section.executable)
      continue;

    auto rangeMatches =
        scanRange(section.virtual_address, section.virtual_address + section.virtual_size,
                  sig.pattern, sig.mask, sig.entryOffset);

    matches.insert(matches.end(), rangeMatches.begin(), rangeMatches.end());
  }

  return matches;
}

std::unordered_map<std::string, std::vector<uint32_t>> SigScanner::scanAll(
    const std::vector<Signature>& sigs) {
  std::unordered_map<std::string, std::vector<uint32_t>> results;

  for (const auto& sig : sigs) {
    results[sig.name] = scan(sig);
  }

  return results;
}

std::vector<uint32_t> SigScanner::scanRange(uint32_t start, uint32_t end,
                                            const std::vector<uint32_t>& pattern,
                                            const std::vector<uint32_t>& mask, size_t entryOffset) {
  std::vector<uint32_t> matches;

  if (pattern.empty() || pattern.size() != mask.size()) {
    return matches;
  }

  const auto* section = module_.FindSectionByAddress(start);
  if (!section || !section->host_data) {
    return matches;
  }

  size_t patternBytes = pattern.size() * 4;
  uint32_t sectionEnd = section->virtual_address + section->virtual_size;
  uint32_t scanEnd = std::min(end, sectionEnd);

  if (start + patternBytes > scanEnd) {
    return matches;
  }

  const uint8_t* data = section->host_data;
  uint32_t sectionBase = section->virtual_address;

  for (uint32_t addr = start; addr + patternBytes <= scanEnd; addr += 4) {
    bool matched = true;
    uint32_t offset = addr - sectionBase;

    for (size_t i = 0; i < pattern.size() && matched; i++) {
      uint32_t dword = memory::load<be_u32>(data + offset + i * 4);
      uint32_t masked = dword & mask[i];
      uint32_t expected = pattern[i] & mask[i];

      if (masked != expected) {
        matched = false;
      }
    }

    if (matched) {
      uint32_t entryPoint = addr + static_cast<uint32_t>(entryOffset * 4);
      matches.push_back(entryPoint);
      REXLOG_TRACE(codegen, "SigScanner: pattern match at 0x{:08X}, entry=0x{:08X}", addr,
                   entryPoint);
    }
  }

  return matches;
}

/**
 * Returns signatures of the register save and restore helpers, matched on their first
 * instruction. Each helper is a run of stw or lwz on rN at an offset from r12 for GPRs and stfd or
 * lfd for FPRs, and the GPR save helper ends in stw r0, 8(r12) and blr. The first instructions
 * are stw r14, -0x48(r12) = 0x91CBFFB8, lwz r14, -0x48(r12) = 0x81CBFFB8,
 * stfd fr14, -0x98(r12) = 0xD9CCFF68 and lfd fr14, -0x98(r12) = 0xC9CCFF68.
 *
 * @return The signatures.
 */
std::vector<Signature> SigScanner::helperSignatures() {
  std::vector<Signature> sigs;

  sigs.push_back({"__savegprlr_14", {0x91CBFFB8}, {0xFFFFFFFF}, 0, std::nullopt});

  sigs.push_back({"__restgprlr_14", {0x81CBFFB8}, {0xFFFFFFFF}, 0, std::nullopt});

  sigs.push_back({"__savefpr_14", {0xD9CCFF68}, {0xFFFFFFFF}, 0, std::nullopt});

  sigs.push_back({"__restfpr_14", {0xC9CCFF68}, {0xFFFFFFFF}, 0, std::nullopt});

  return sigs;
}

std::vector<Signature> SigScanner::hleSignatures() {
  return {};
}

}  // namespace rex::codegen
