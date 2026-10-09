/**
 * @file        ppc/func.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Split out of rex/ppc/context.h so callers that only need the
 *              PPCFunc type do not pull in PPCContext and its SIMD headers.
 */

#pragma once

#include <csetjmp>
#include <cstddef>
#include <cstdint>

#include <rex/macros.h>

//=============================================================================
// PPCFunc Type Definition
//=============================================================================
// Function signature for recompiled PPC functions.
// All recompiled functions take a context reference and memory base pointer.

// Forward declaration of the PPC execution context
struct PPCContext;

// Function signature for recompiled PPC functions
using PPCFunc = void(PPCContext& ctx, uint8_t* base);

namespace rex::runtime {
REX_RUNTIME_API PPCFunc* ResolveIndirectFunction(uint32_t guest_address);
}  // namespace rex::runtime

namespace rex::ppc {
/**
 * Handles a trap instruction of generated code.
 *
 * @param ctx The context of the trapping thread.
 * @param base The guest memory base.
 * @param trap_type The trap type the instruction encodes.
 */
REX_RUNTIME_API void Trap(PPCContext& ctx, uint8_t* base, uint16_t trap_type);

/**
 * Returns the host jump buffer of the calling thread for a title jump buffer.
 *
 * @param address The guest address of the jump buffer.
 * @return The host jump buffer, created on first use.
 */
REX_RUNTIME_API std::jmp_buf& JumpBuffer(uint32_t address);

/**
 * Jumps to the host jump buffer of the calling thread for a title jump buffer.
 *
 * @param address The guest address of the jump buffer.
 * @param value The value setjmp returns.
 */
[[noreturn]] REX_RUNTIME_API void LongJump(uint32_t address, int value);

/**
 * Throws for an instruction that codegen does not translate.
 *
 * @param opcode The instruction name.
 */
[[noreturn]] REX_RUNTIME_API void Unimplemented(const char* opcode);
}  // namespace rex::ppc

//=============================================================================
// PPC Function Macros
//=============================================================================

#define REX_JOIN(x, y) x##y
#define REX_XSTRINGIFY(x) #x
#define REX_STRINGIFY(x) REX_XSTRINGIFY(x)
// REX_FUNC is the bare signature; the declaring site supplies linkage, either
// by writing extern "C" itself or by using REX_EXTERN.
#define REX_FUNC(x) void x([[maybe_unused]] PPCContext& __restrict ctx, uint8_t* base)
#define REX_EXTERN(x) extern "C" REX_FUNC(x)

//=============================================================================
// Function Mapping
//=============================================================================

struct PPCFuncMapping {
  size_t guest;
  PPCFunc* host;
};

extern PPCFuncMapping PPCFuncMappings[];
