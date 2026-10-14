/**
 * @file        codegen/sig_scanner.h
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

#include <rex/runtime/module.h>

namespace rex::codegen {

/**
 * Pattern definition for matching. Patterns are dword based, following the PPC instruction
 * size. Each pattern word is matched against memory through the corresponding mask, where
 * 0xFFFFFFFF is an exact match and a partial mask allows wildcards.
 */
struct Signature {
  std::string name;           /**< Symbol name such as "__savegprlr_14" */
  std::vector<u32> pattern;   /**< Instruction words to match */
  std::vector<u32> mask;      /**< Bits that must match, 0xFFFFFFFF for exact */
  size_t entry_offset = 0;    /**< Offset from the pattern start to the entry point */
  std::optional<size_t> size; /**< Known size, or nullopt to compute from the pattern */
};

/** Pattern based signature matcher. */
class SigScanner {
 public:
  /**
   * Creates a scanner over a module.
   *
   * @param module Module to scan, must outlive the scanner
   */
  explicit SigScanner(const runtime::Module& module);

  /**
   * Scans for a single signature.
   *
   * @param sig Signature to match
   * @return Entry points of all matches
   */
  std::vector<u32> Scan(const Signature& sig);

  /**
   * Scans for multiple signatures in a single pass.
   *
   * @param sigs Signatures to match
   * @return Entry points of all matches keyed by signature name
   */
  std::unordered_map<std::string, std::vector<u32>> ScanAll(const std::vector<Signature>& sigs);

  /**
   * Returns the built in signatures of the __save and __restore helpers.
   *
   * @return Helper signatures
   */
  static std::vector<Signature> GetHelperSignatures();

  /**
   * Returns the built in signatures of memset, memmove and other library routines.
   *
   * @return Library signatures
   */
  static std::vector<Signature> GetHLESignatures();

 private:
  const runtime::Module& module_;

  std::vector<u32> ScanRange(u32 start, u32 end, const std::vector<u32>& pattern,
                             const std::vector<u32>& mask, size_t entry_offset);
};

}  // namespace rex::codegen
