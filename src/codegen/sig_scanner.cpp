#include <rex/codegen/sig_scanner.h>
#include <rex/logging.h>

#include <rex/memory/utils.h>
#include <rex/types.h>

namespace rex::codegen {

SigScanner::SigScanner(const runtime::Module& module) : module_(module) {}

std::vector<u32> SigScanner::scan(const Signature& sig) {
  std::vector<u32> matches;

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

std::unordered_map<std::string, std::vector<u32>> SigScanner::scanAll(
    const std::vector<Signature>& sigs) {
  std::unordered_map<std::string, std::vector<u32>> results;

  for (const auto& sig : sigs) {
    results[sig.name] = scan(sig);
  }

  return results;
}

std::vector<u32> SigScanner::scanRange(u32 start, u32 end, const std::vector<u32>& pattern,
                                       const std::vector<u32>& mask, size_t entryOffset) {
  std::vector<u32> matches;

  if (pattern.empty() || pattern.size() != mask.size()) {
    return matches;
  }

  const auto* section = module_.FindSectionByAddress(start);
  if (!section || !section->host_data) {
    return matches;
  }

  size_t patternBytes = pattern.size() * 4;
  u32 sectionEnd = section->virtual_address + section->virtual_size;
  u32 scanEnd = std::min(end, sectionEnd);

  if (start + patternBytes > scanEnd) {
    return matches;
  }

  const u8* data = section->host_data;
  u32 sectionBase = section->virtual_address;

  for (u32 addr = start; addr + patternBytes <= scanEnd; addr += 4) {
    bool matched = true;
    u32 offset = addr - sectionBase;

    for (size_t i = 0; i < pattern.size() && matched; i++) {
      u32 dword = memory::load<be_u32>(data + offset + i * 4);
      u32 masked = dword & mask[i];
      u32 expected = pattern[i] & mask[i];

      if (masked != expected) {
        matched = false;
      }
    }

    if (matched) {
      u32 entryPoint = addr + static_cast<u32>(entryOffset * 4);
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
