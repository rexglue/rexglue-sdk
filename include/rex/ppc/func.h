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

struct PPCContext;

/**
 * Signature of recompiled PPC functions. Every recompiled function takes a context reference
 * and the memory base pointer.
 */
using PPCFunc = void(PPCContext& ctx, u8* base);

namespace rex::runtime {
/**
 * Resolves a guest address to the recompiled function that implements it.
 *
 * @param guest_address The guest address.
 * @return The function, or a trap function when the address has none.
 */
REX_RUNTIME_API PPCFunc* ResolveIndirectFunction(u32 guest_address);
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

/**
 * Joins 2 tokens into 1 without expanding them first.
 *
 * @param x The left tokens.
 * @param y The right tokens.
 *
 * @code
 * REX_JOIN(prefix_, name)
 * @endcode
 */
#define REX_JOIN(x, y) x##y
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
/**
 * Declares a recompiled function. Expands to the bare signature, so the declaring site supplies
 * linkage by writing extern "C" itself or by using REX_EXTERN.
 *
 * @param x The function name.
 *
 * @code
 * REX_FUNC(sub_82010000);
 * @endcode
 */
#define REX_FUNC(x) void x([[maybe_unused]] PPCContext& __restrict ctx, u8* base)

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
  size_t guest;  /**< Guest address of the function. */
  PPCFunc* host; /**< Recompiled function. */
};

extern PPCFuncMapping PPCFuncMappings[];
