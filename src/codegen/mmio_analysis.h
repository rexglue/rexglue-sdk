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

#include <rex/codegen/config.h>
#include <rex/codegen/function_types.h>
#include <rex/types.h>

namespace rex::codegen {

/** How generated code reaches the memory an instruction accesses. */
enum class MMIOAccess : u8 {
  /** A plain load or store with no check. */
  kPlain,
  /** A direct call into the GPU recipient. */
  kGPU,
  /** A direct call into the APU recipient. */
  kAPU,
  /** A load or store that tests for a register block first. */
  kGuarded,
};

/** Why an access has a report entry. */
enum class MMIOReason : u8 {
  /** The effective address is a known register block address. */
  kProven,
  /** The effective address is an unknown value shifted plus a register block constant. */
  kOffset,
  /** The base is unknown and eieio follows the access. */
  kBarrier,
  /** The [mmio.gpu] or [mmio.apu] table lists the instruction. */
  kDeclared,
  /** The effective address is a known 0x7F address outside both blocks. */
  kUnserved,
  /** A register block address reached by an instruction the builders do not route. */
  kUnsupported,
};

/** A load or store with a report entry. */
struct MMIOSite {
  /** Guest address of the instruction. */
  u32 address;
  /** The access the builders emit. */
  MMIOAccess access;
  /** Why the site has an entry. */
  MMIOReason reason;
  /** The effective address when it is known, otherwise 0. */
  u32 target;
  /** True when the instruction writes memory. */
  bool write;
};

/** A site with the function that holds it, as the report lists it. */
struct MMIOReportSite {
  /** Guest address of the function. */
  u32 function;
  /** The site. */
  MMIOSite site;
};

/** Contiguous code of the function being classified. */
struct MMIOCodeBlock {
  /** Guest address of the first instruction. */
  u32 base;
  /** Instruction words in image byte order. */
  std::span<const u8> bytes;
};

/** Everything the analysis reads for 1 function. */
struct MMIOFunctionInput {
  /** Code of the function. The first block starts at the function entry. */
  std::span<const MMIOCodeBlock> blocks;
  /** Jump tables of the function, keyed by their bctr_address. */
  std::span<const JumpTable> jump_tables;
  /** Instruction addresses that carry a mid asm hook. */
  std::span<const u32> hook_sites;
  /** Addresses a mid asm hook may jump to. */
  std::span<const u32> hook_targets;
  /** Loads and stores from the [mmio.gpu] and [mmio.apu] tables, or null. */
  const std::unordered_map<u32, MMIODeclaredAccess>* declared = nullptr;
};

/** Classification of every load and store in a function. */
struct MMIOClassification {
  /** Every site with a report entry, in address order. */
  std::vector<MMIOSite> sites;

  /**
   * Looks up the access an instruction needs.
   *
   * @param address Guest address of the instruction.
   * @return The access of the site at the address, or Plain when there is none.
   */
  MMIOAccess At(u32 address) const;
};

/** File name of the MMIO report in the codegen output directory. */
inline constexpr std::string_view kMMIOReportFileName = "codegen.mmio.json";

/**
 * Classifies the loads and stores of 1 function by the value each base register holds.
 *
 * @param input The code, jump tables, hooks and declared sites of the function.
 * @return Every site with a report entry, in address order.
 */
MMIOClassification ClassifyMMIO(const MMIOFunctionInput& input);

}  // namespace rex::codegen
