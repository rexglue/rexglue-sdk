/**
 * @file        rex/codegen/sig_scanner.h
 * @brief       Signature scanner for pattern-based function discovery
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
#include <unordered_map>
#include <vector>

#include <rex/system/module.h>

namespace rex::codegen {

//=============================================================================
// Signature - Pattern definition for matching
//=============================================================================
// Dword-based patterns follow PPC's per-instruction size.
// Each pattern word is matched against memory using the corresponding mask.
// A mask of 0xFFFFFFFF means exact match; partial masks allow wildcards.

struct Signature {
  std::string name;               // e.g., "__savegprlr_14"
  std::vector<uint32_t> pattern;  // Instruction words to match
  std::vector<uint32_t> mask;     // Bits that must match (0xFFFFFFFF = exact)
  size_t entryOffset = 0;         // Offset from pattern start to entry point
  std::optional<size_t> size;     // Known size, or nullopt to compute from pattern
};

//=============================================================================
// SigScanner - Pattern-based signature matcher
//=============================================================================

class SigScanner {
 public:
  explicit SigScanner(const runtime::Module& module);

  // Scan for a single signature, return all match entry points
  std::vector<uint32_t> scan(const Signature& sig);

  // Scan for multiple signatures at once (more efficient - single pass)
  std::unordered_map<std::string, std::vector<uint32_t>> scanAll(
      const std::vector<Signature>& sigs);

  // Built-in signature sets
  static std::vector<Signature> helperSignatures();  // __save/__restore helpers
  static std::vector<Signature> hleSignatures();     // memset, memmove, etc. (future)

 private:
  const runtime::Module& module_;

  // Scan a single range for a pattern
  std::vector<uint32_t> scanRange(uint32_t start, uint32_t end,
                                  const std::vector<uint32_t>& pattern,
                                  const std::vector<uint32_t>& mask, size_t entryOffset);
};

}  // namespace rex::codegen
