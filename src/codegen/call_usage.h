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

#include <bitset>
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

  bool operator==(const CallUsage&) const = default;
};

/**
 * The non volatile registers live at the entry of a function: the ones it reads before it writes
 * them, which hold what its caller left there.
 */
struct NonVolatileLiveIns {
  u32 gpr = 0;         /**< Bit n is set when the function reads rn, r14 to r31. */
  u32 fpr = 0;         /**< Bit n is set when the function reads fn, f14 to f31. */
  std::bitset<128> vr; /**< Bit n is set when the function reads vn, v14 to v31 and v64 to v127. */

  bool operator==(const NonVolatileLiveIns&) const = default;

  friend NonVolatileLiveIns operator|(const NonVolatileLiveIns& a, const NonVolatileLiveIns& b) {
    return {a.gpr | b.gpr, a.fpr | b.fpr, a.vr | b.vr};
  }
  friend NonVolatileLiveIns operator&(const NonVolatileLiveIns& a, const NonVolatileLiveIns& b) {
    return {a.gpr & b.gpr, a.fpr & b.fpr, a.vr & b.vr};
  }
  friend NonVolatileLiveIns operator~(const NonVolatileLiveIns& a) {
    return {~a.gpr, ~a.fpr, ~a.vr};
  }
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

/**
 * Finds the non volatile registers a function reads before it writes them, by a backward liveness
 * analysis that runs to a fixpoint.
 *
 * A direct call or tail branch to a function in callees reads what that function reads. A call
 * leaves the non volatile registers alone, and a call to an address in opaque, or an indirect call,
 * reads none. A store through r1 saves a register in the frame, as a prologue does, and is not a
 * read. A register the function writes, other than by a load through r1 (an epilogue's reload),
 * is left out: the function sets it, and a path that reads it first is one the code does not take.
 *
 * @param opaque The addresses whose calls read no register: the register save and restore entries,
 *     and any other function the caller wants left out.
 * @param callees The registers read by each analyzed function, by entry address.
 * @return The registers read, empty for an import or a function without instructions.
 */
NonVolatileLiveIns ComputeNonVolatileLiveIn(
    const FunctionNode& fn, const std::unordered_set<u32>& opaque,
    const std::unordered_map<u32, NonVolatileLiveIns>& callees = {});

/**
 * Finds the non volatile registers every given function reads before it writes them, counting the
 * registers a direct callee reads. Mutually recursive functions settle at the registers any of
 * them reads.
 *
 * @param opaque The addresses whose calls read no register, as for ComputeNonVolatileLiveIn.
 * @return The registers read by each function that reads any, by entry address.
 */
std::unordered_map<u32, NonVolatileLiveIns> ComputeNonVolatileLiveIns(
    std::span<const FunctionNode* const> functions, const std::unordered_set<u32>& opaque);

}  // namespace rex::codegen
