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

#include <rex/platform.h>
#include <rex/export.h>
#include <rex/types.h>

struct PPCContext;

// Function signature for recompiled PPC functions
using PPCFunc = void(PPCContext& ctx, u8* base);

namespace rex::ppc {
/**
 * Resolves a guest address to the recompiled function that implements it.
 *
 * @param guest_address The guest address.
 * @return The function, or a trap function when the address has none.
 */
REX_RUNTIME_API PPCFunc* ResolveFunction(u32 guest_address);

/**
 * Handles a trap instruction of generated code.
 *
 * @param ctx The context of the trapping thread.
 * @param base The guest memory base.
 * @param trap_type The trap type the instruction encodes.
 */
REX_RUNTIME_API void Trap(PPCContext& ctx, u8* base, u16 trap_type);

/**
 * Returns the host jump buffer of the calling thread for a title jump buffer.
 *
 * @param address The guest address of the jump buffer.
 * @return The host jump buffer, created on first use.
 */
REX_RUNTIME_API std::jmp_buf& GetJumpBuffer(u32 address);

/**
 * Jumps to the host jump buffer of the calling thread for a title jump buffer.
 *
 * @param address The guest address of the jump buffer.
 * @param value The value setjmp returns.
 */
[[noreturn]] REX_RUNTIME_API void LongJump(u32 address, int value);

/**
 * Throws for an instruction that codegen does not translate.
 *
 * @param opcode The instruction name.
 */
[[noreturn]] REX_RUNTIME_API void UnimplementedInstruction(const char* opcode);
}  // namespace rex::ppc

/**
 * Turns the argument into a string literal without expanding it first.
 *
 * @param x The tokens to stringify.
 *
 * @code
 * REX_XSTRINGIFY(__LINE__)
 * @endcode
 */
#define REX_XSTRINGIFY(x) #x
/**
 * Expands the argument, then turns it into a string literal.
 *
 * @param x The tokens to expand and stringify.
 *
 * @code
 * REX_STRINGIFY(__LINE__)
 * @endcode
 */
#define REX_STRINGIFY(x) REX_XSTRINGIFY(x)
// REX_FUNC is the bare signature; the declaring site supplies linkage, either
// by writing extern "C" itself or by using REX_EXTERN.
#define REX_FUNC(x) void x([[maybe_unused]] PPCContext& __restrict ctx, u8* base)
#define REX_EXTERN(x) extern "C" REX_FUNC(x)

/**
 * Declares a recompiled function with C linkage.
 *
 * @param x The function name.
 *
 * @code
 * REX_EXTERN(sub_82010000);
 * @endcode
 */
#define REX_EXTERN(x) extern "C" REX_FUNC(x)

/** Maps a guest function address to its recompiled function. */
struct PPCFuncMapping {
  size_t address; /**< Entry point of the function in guest memory. */
  PPCFunc* host;  /**< Recompiled function. */
};

/** Guest to host function mappings, defined by the generated code. */
extern const PPCFuncMapping kPPCFuncMappings[];
