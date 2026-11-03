/**
 * @file        codegen/function_scanner.h
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
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "config.h"
#include "function_graph.h"
#include <rex/result.h>
#include <rex/types.h>

namespace rex::codegen {

struct Limits;

/** Half open address range of code, from start up to but excluding end. */
struct CodeRegion {
  u32 start;
  u32 end; /**< Address 1 past the range */

  bool Contains(u32 addr) const { return addr >= start && addr < end; }
  u32 GetSize() const { return end - start; }
};
class BinaryView;
class DecodedBinary;

/** Blocks, branches and instructions found by DiscoverBlocks() for one function. */
struct BlockDiscoveryResult {
  BlockDiscoveryResult() = default;
  BlockDiscoveryResult(const BlockDiscoveryResult&) = default;
  BlockDiscoveryResult& operator=(const BlockDiscoveryResult&) = default;
  BlockDiscoveryResult(BlockDiscoveryResult&&) noexcept(false) = default;
  BlockDiscoveryResult& operator=(BlockDiscoveryResult&&) noexcept(false) = default;
  ~BlockDiscoveryResult() = default;

  std::vector<Block> blocks;
  std::vector<UnresolvedJump> unresolved_jumps; /**< Jumps left for the graph to resolve */
  std::vector<JumpTable> jump_tables;
  std::set<u32> labels; /**< Internal branch target addresses */

  std::vector<rex::codegen::detail::Instruction*> instructions; /**< Handed to FunctionNode */

  std::vector<u32> external_calls; /**< bl to unknown targets */
  std::vector<u32> tail_calls;     /**< b to external targets */
  /** Entries of the known functions that a block runs into. */
  std::set<u32> fall_throughs;
};

/**
 * Discovers the blocks of a function from its entry point. The scan sweeps linearly to a
 * terminator (blr, bctr or unconditional b), follows both paths of a conditional branch, detects
 * jump tables at bctr instructions and stops at code region boundaries.
 *
 * @param known_functions Known function entry points, to detect tail calls
 * @param pdata_size Function size in bytes from PDATA, or 0 when unknown
 * @param manual_switch_tables Configured switch tables keyed by bctr address, or null
 * @return The blocks, branches and jump tables
 */
BlockDiscoveryResult DiscoverBlocks(
    DecodedBinary& decoded, const Limits& limits, u32 entry_point,
    const CodeRegion& containing_region, const std::unordered_set<u32>& known_functions,
    u32 pdata_size = 0, const std::unordered_map<u32, JumpTable>* manual_switch_tables = nullptr);

/**
 * Finds where a function without a declared extent ends at a return. A return is a cut when no
 * conditional branch or jump table entry located before it targets an address after it, and the
 * code after it does not branch back into the part before it.
 *
 * @param result Blocks, instructions and jump tables found from the entry.
 * @return The address after the lowest cut return, or no value when there is none.
 */
std::optional<u32> FindReturnCut(const BlockDiscoveryResult& result, u32 entry_point);

/**
 * Detects the jump table at a bctr instruction. The patterns are ABSOLUTE (lwzx loads full 32 bit
 * addresses), COMPUTED (lbzx and rlwinm, a byte offset with shift), BYTEOFFSET (lbzx and add) and
 * SHORTOFFSET (lhzx and add, a 16 bit offset).
 *
 * @param containing_region Code region for validation
 * @return JumpTable if detected, empty optional otherwise
 */
std::optional<JumpTable> DetectJumpTable(DecodedBinary& decoded, const Limits& limits,
                                         u32 bctr_addr, const CodeRegion& containing_region,
                                         u32 func_start, u32 func_end);

}  // namespace rex::codegen
