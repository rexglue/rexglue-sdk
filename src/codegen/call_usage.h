/**
 * @file        codegen/call_usage.h
 * @brief       The argument registers a recompiled function reads
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <span>
#include <unordered_map>
#include <unordered_set>

#include "function_node.h"
#include <rex/types.h>

namespace rex::codegen {

/** The argument registers live at the entry of a function. Bit 0 is r3 and f1. */
struct CallUsage {
  u8 gpr_mask = 0;  /**< Bit n is set when the function reads r(3 + n), r3 to r10. */
  u16 fpr_mask = 0; /**< Bit n is set when the function reads f(1 + n), f1 to f13. */
};

/**
 * Finds the argument registers a function reads before it writes them, by a backward liveness
 * analysis that runs to a fixpoint.
 *
 * A direct call or tail branch to a function in callees reads what that function reads, and a call
 * writes r3 to r12 and f0 to f13. An indirect call reads nothing. A call to a register save or
 * restore entry touches no argument register. A store through r1 at a positive displacement before
 * the frame is allocated spills an argument register to the caller's parameter area and is not a
 * read.
 *
 * @param millicode The addresses of the register save and restore entries.
 * @param callees The registers read by each analyzed function, by entry address.
 * @return The registers read, empty for an import or a function without instructions.
 */
CallUsage ComputeCallUsage(const FunctionNode& fn, const std::unordered_set<u32>& millicode,
                           const std::unordered_map<u32, CallUsage>& callees = {});

/**
 * Finds the argument registers every given function reads, counting a register a function only
 * passes on to a direct callee. Mutually recursive functions settle at the registers any of them
 * reads.
 *
 * @param millicode The addresses of the register save and restore entries.
 * @return The registers read by each function that reads any, by entry address.
 */
std::unordered_map<u32, CallUsage> ComputeCallUsages(std::span<const FunctionNode* const> functions,
                                                     const std::unordered_set<u32>& millicode);

}  // namespace rex::codegen
