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

#include <rex/codegen/config.h>
#include <rex/codegen/function_graph.h>
#include <rex/codegen/limits.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex::codegen {

struct CodeRegion;
class BinaryView;
class DecodedBinary;

/** Blocks, branches and instructions found by DiscoverBlocks() for 1 function. */
struct BlockDiscoveryResult {
  BlockDiscoveryResult() = default;
  /** Copy and move construction and assignment. @{ */
  BlockDiscoveryResult(const BlockDiscoveryResult&) = default;
  BlockDiscoveryResult& operator=(const BlockDiscoveryResult&) = default;
  BlockDiscoveryResult(BlockDiscoveryResult&&) noexcept(false) = default;
  BlockDiscoveryResult& operator=(BlockDiscoveryResult&&) noexcept(false) = default;
  /** @} */
  ~BlockDiscoveryResult() = default;

  std::vector<Block> blocks;                    /**< Discovered blocks */
  std::vector<UnresolvedJump> unresolved_jumps; /**< Jumps left for the graph to resolve */
  std::vector<JumpTable> jump_tables;           /**< Detected jump tables */
  std::set<u32> labels;                         /**< Internal branch target addresses */

  std::vector<rex::codegen::detail::Instruction*> instructions; /**< Handed to FunctionNode */

  std::vector<u32> external_calls; /**< bl to unknown targets */
  std::vector<u32> tail_calls;     /**< b to external targets */
  /** Entries of the known functions that a block runs into. */
  std::set<u32> fall_throughs;
};

/**
 * Discover all blocks belonging to a function starting at entry_point.
 *
 * The scan runs in these steps.
 * - Worklist based block discovery
 * - Linear sweep until terminator (blr, bctr, unconditional b)
 * - Follow both paths for conditional branches
 * - Detect jump tables at bctr instructions
 * - Stop at code region boundaries (null padding)
 *
 * @param decoded The decoded binary (single pass decoded instructions)
 * @param limits Analysis limits
 * @param entry_point Starting address of the function
 * @param containing_region Code region containing the entry point
 * @param known_functions Set of known function entry points (to detect tail calls)
 * @param pdata_size Function size in bytes from PDATA, or 0 when unknown
 * @param manual_switch_tables Configured switch tables keyed by bctr address, or null
 * @return BlockDiscoveryResult containing blocks, branches, and jump tables
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
 * @param entry_point Function entry address.
 * @return The address after the lowest cut return, or no value when there is none.
 */
std::optional<u32> FindReturnCut(const BlockDiscoveryResult& result, u32 entry_point);

/**
 * Detect jump table at a bctr instruction.
 *
 * Detected patterns.
 * - ABSOLUTE, lwzx loads full 32 bit addresses
 * - COMPUTED, lbzx + rlwinm (byte offset with shift)
 * - BYTEOFFSET, lbzx + add (byte offset direct)
 * - SHORTOFFSET, lhzx + add (16 bit offset)
 *
 * @param decoded The decoded binary
 * @param limits Analysis limits
 * @param bctr_addr Address of the bctr instruction
 * @param containing_region Code region for validation
 * @param func_start Function start address, lower bound for table targets
 * @param func_end Function end address
 * @return JumpTable if detected, empty optional otherwise
 */
std::optional<JumpTable> DetectJumpTable(DecodedBinary& decoded, const Limits& limits,
                                         u32 bctr_addr, const CodeRegion& containing_region,
                                         u32 func_start, u32 func_end);

}  // namespace rex::codegen
