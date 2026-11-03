/**
 * @file        codegen/mmio_analysis.h
 * @brief       Recompile time classification of MMIO loads and stores
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "function_types.h"
#include <rex/types.h>

namespace rex::codegen {

/** How generated code reaches the memory an instruction accesses. */
enum class MMIOAccess : u8 {
  kPlain,   /**< A plain load or store with no check. */
  kGPU,     /**< A direct call into the GPU. */
  kAPU,     /**< A direct call into the APU. */
  kGuarded, /**< A load or store that tests for a register block first. */
};

/** Why an access has a report entry. */
enum class MMIOReason : u8 {
  kProven,      /**< The effective address is a known register block address. */
  kOffset,      /**< An unknown shifted value plus a register block constant. */
  kBarrier,     /**< The base is unknown and eieio follows the access. */
  kDeclared,    /**< The [mmio.gpu] or [mmio.apu] table lists the instruction. */
  kUnserved,    /**< A known 0x7F address outside both blocks. */
  kUnsupported, /**< A register block address reached by an instruction the builders do not route.
                 */
};

/** A load or store with a report entry. */
struct MMIOSite {
  u32 address; /**< Guest address of the instruction. */
  MMIOAccess access;
  MMIOReason reason;
  u32 target; /**< The effective address when known, otherwise 0. */
  bool write;
};

/** A site with the function that holds it, as the report lists it. */
struct MMIOReportSite {
  u32 function; /**< Guest address of the function. */
  MMIOSite site;
};

/** Contiguous code of the function being classified. */
struct MMIOCodeBlock {
  u32 base;                  /**< Guest address of the first instruction. */
  std::span<const u8> bytes; /**< Instruction words in image byte order. */
};

/** Everything the analysis reads for one function. */
struct MMIOFunctionInput {
  std::span<const MMIOCodeBlock> blocks; /**< The first block starts at the function entry. */
  std::span<const JumpTable> jump_tables;
  std::span<const u32> hook_sites;   /**< Instruction addresses that carry a mid asm hook. */
  std::span<const u32> hook_targets; /**< Addresses a mid asm hook may jump to. */
  /** Loads and stores from the [mmio.gpu] and [mmio.apu] tables, or null. */
  const std::unordered_map<u32, MMIODeclaredAccess>* declared = nullptr;
};

/** Classification of every load and store in a function. */
struct MMIOClassification {
  std::vector<MMIOSite> sites; /**< In address order. */

  /** Returns the access an instruction needs, or Plain when there is none. */
  MMIOAccess At(u32 address) const;
};

/** File name of the MMIO report in the codegen output directory. */
inline constexpr std::string_view kMMIOReportFileName = "codegen.mmio.json";

/**
 * Classifies the loads and stores of one function by the value each base register holds.
 *
 * @return Every site with a report entry, in address order.
 */
MMIOClassification ClassifyMMIO(const MMIOFunctionInput& input);

}  // namespace rex::codegen
