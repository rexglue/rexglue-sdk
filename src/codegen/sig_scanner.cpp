/**
 * @file        rexcodegen/sig_scanner.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/codegen/sig_scanner.h>
#include <rex/logging.h>

#include "codegen_logging.h"
#include <rex/memory/utils.h>
#include <rex/types.h>

using rex::memory::load_and_swap;

namespace rex::codegen {

SigScanner::SigScanner(const runtime::Module& module) : module_(module) {}

std::vector<uint32_t> SigScanner::scan(const Signature& sig) {
  std::vector<uint32_t> matches;

  // Scan all executable sections
  // TODO(tomc): maybe i wanna scan other sections...
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

  // Find section containing this range
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

  // Scan through the range (4-byte aligned for PPC)
  for (uint32_t addr = start; addr + patternBytes <= scanEnd; addr += 4) {
    bool matched = true;
    uint32_t offset = addr - sectionBase;

    for (size_t i = 0; i < pattern.size() && matched; i++) {
      uint32_t dword = load_and_swap<uint32_t>(data + offset + i * 4);
      uint32_t masked = dword & mask[i];
      uint32_t expected = pattern[i] & mask[i];

      if (masked != expected) {
        matched = false;
      }
    }

    if (matched) {
      uint32_t entryPoint = addr + static_cast<uint32_t>(entryOffset * 4);
      matches.push_back(entryPoint);
      REXCODEGEN_TRACE("SigScanner: pattern match at 0x{:08X}, entry=0x{:08X}", addr, entryPoint);
    }
  }

  return matches;
}

std::vector<Signature> SigScanner::helperSignatures() {
  std::vector<Signature> sigs;

  // __savegprlr_14 pattern:
  // The save helpers are a sequence of stw rN, offset(r12) instructions
  // followed by stw r0, 8(r12) and blr
  //
  // stw rN, offset(r12) = 0x9180XXXX where XX encodes the offset
  // For r14: stw r14, -0x48(r12) = 0x91CBFFB8
  //
  // We look for the first instruction of the sequence.
  // stw r14, -0x48(r12) = 0x91CBFFB8

  sigs.push_back({
      "__savegprlr_14",
      {0x91CBFFB8},  // stw r14, -0x48(r12)
      {0xFFFFFFFF},  // Exact match
      0,             // Entry at pattern start
      std::nullopt   // Size computed from stride
  });

  // __restgprlr_14 pattern:
  // lwz rN, offset(r12) followed by eventually mtlr r0 and blr
  // lwz r14, -0x48(r12) = 0x81CBFFB8

  sigs.push_back({"__restgprlr_14",
                  {0x81CBFFB8},  // lwz r14, -0x48(r12)
                  {0xFFFFFFFF},  // Exact match
                  0,
                  std::nullopt});

  // __savefpr_14 pattern:
  // stfd frN, offset(r12)
  // stfd fr14, -0x98(r12) = 0xD9CCFF68

  sigs.push_back({"__savefpr_14",
                  {0xD9CCFF68},  // stfd fr14, -0x98(r12)
                  {0xFFFFFFFF},
                  0,
                  std::nullopt});

  // __restfpr_14 pattern:
  // lfd frN, offset(r12)
  // lfd fr14, -0x98(r12) = 0xC9CCFF68

  sigs.push_back({"__restfpr_14",
                  {0xC9CCFF68},  // lfd fr14, -0x98(r12)
                  {0xFFFFFFFF},
                  0,
                  std::nullopt});

  return sigs;
}

std::vector<Signature> SigScanner::hleSignatures() {
  // TODO(tomc): mayhaps have signatures memset, memmove, memcpy, strcmp patterns
  return {};
}

}  // namespace rex::codegen
