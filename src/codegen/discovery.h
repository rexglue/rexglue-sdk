/**
 * @file        rex/codegen/discovery.h
 * @brief       Block discovery using DecodedBinary
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include "decoded_binary.h"

#include <cstdint>
#include <set>
#include <unordered_set>
#include <vector>

#include <rex/codegen/function_graph.h>

namespace rex::codegen {

//=============================================================================
// Block Discovery Result
//=============================================================================

struct UnresolvedBranch {
  uint32_t site;       // Address of branch instruction
  uint32_t target;     // Target address
  bool isCall;         // true = bl (call), false = b (tail/jump)
  bool isConditional;  // true = bc/beq/etc, false = unconditional
};

struct BlockDiscoveryResult {
  std::vector<Block> blocks;
  std::vector<UnresolvedBranch> unresolvedBranches;
  std::vector<JumpTable> jumpTables;
  std::set<uint32_t> labels;

  // Collected instruction pointers (for FunctionNode ownership)
  std::vector<rex::codegen::ppc::Instruction*> instructions;

  // External references found during discovery
  std::vector<uint32_t> externalCalls;  // bl to unknown targets
  std::vector<uint32_t> tailCalls;      // b to external targets
};

//=============================================================================
// Block Discovery Function
//=============================================================================

/**
 * Discover all blocks belonging to a function starting at entryPoint.
 *
 * Algorithm:
 * - Worklist-based block discovery
 * - Linear sweep until terminator (blr, bctr, unconditional b)
 * - Follow both paths for conditional branches
 * - Detect jump tables at bctr instructions
 * - Stop at code region boundaries (null padding)
 *
 * @param decoded The decoded binary (single-pass decoded instructions)
 * @param entryPoint Starting address of the function
 * @param containingRegion Code region containing the entry point
 * @param knownFunctions Set of known function entry points (to detect tail calls)
 * @return BlockDiscoveryResult containing blocks, branches, and jump tables
 */
BlockDiscoveryResult discoverBlocks(DecodedBinary& decoded, uint32_t entryPoint,
                                    const CodeRegion& containingRegion,
                                    const std::unordered_set<uint32_t>& knownFunctions,
                                    uint32_t pdataSize = 0);

//=============================================================================
// Jump Table Detection
//=============================================================================

/**
 * Detect jump table at a bctr instruction.
 *
 * Patterns detected:
 * - ABSOLUTE: lwzx loads full 32-bit addresses
 * - COMPUTED: lbzx + rlwinm (byte offset with shift)
 * - BYTEOFFSET: lbzx + add (byte offset direct)
 * - SHORTOFFSET: lhzx + add (16-bit offset)
 *
 * @param decoded The decoded binary
 * @param bctrAddr Address of the bctr instruction
 * @param containingRegion Code region for validation
 * @return JumpTable if detected, empty optional otherwise
 */
std::optional<JumpTable> detectJumpTable(DecodedBinary& decoded, uint32_t bctrAddr,
                                         const CodeRegion& containingRegion, uint32_t funcStart,
                                         uint32_t funcEnd);

}  // namespace rex::codegen
