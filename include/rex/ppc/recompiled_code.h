/**
 * @file        ppc/recompiled_code.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <csetjmp>
#include <cstddef>
#include <span>

#include <rex/platform.h>
#include <rex/types.h>

#if REX_PLATFORM_WIN32
#include <rex/platform/dynlib.h>
#endif

struct PPCContext;

using PPCFunc = void(PPCContext& ctx, u8* base);

namespace rex::ppc {
/**
 * Resolves a guest address to the recompiled function that implements it.
 *
 * @return The function, or a trap function when the address has none.
 */
REX_PPC_API PPCFunc* ResolveFunction(u32 guest_address);

/** Handles a trap instruction of generated code. */
REX_PPC_API void Trap(PPCContext& ctx, u8* base, u16 trap_type);

/**
 * Returns the host jump buffer of the calling thread for a guest jump buffer address, created on
 * first use.
 *
 * @param address The guest address of the jump buffer.
 */
REX_PPC_API std::jmp_buf& GetJumpBuffer(u32 address);

/**
 * Jumps to the host jump buffer of the calling thread for a guest jump buffer address.
 *
 * @param address The guest address of the jump buffer.
 * @param value The value setjmp returns.
 */
[[noreturn]] REX_PPC_API void LongJump(u32 address, int value);

/** Throws for an instruction that codegen does not translate. */
[[noreturn]] REX_PPC_API void UnimplementedInstruction(const char* opcode);
}  // namespace rex::ppc

/**
 * Declares a recompiled function with no linkage specifier.
 *
 * @param x The C++ symbol the generated code defines and calls.
 *
 * @code
 * REX_FUNC(sub_82010000);
 * @endcode
 */
#define REX_FUNC(x) void x([[maybe_unused]] PPCContext& __restrict ctx, [[maybe_unused]] u8* base)

/**
 * Declares a recompiled function with C linkage.
 *
 * @param x The C++ symbol the generated code defines and calls.
 *
 * @code
 * REX_EXTERN(sub_82010000);
 * @endcode
 */
#define REX_EXTERN(x) extern "C" REX_FUNC(x)

struct PPCFuncMapping {
  size_t address;
  PPCFunc* host;
};

/** Guest to host function mappings, defined by the generated code. */
extern const PPCFuncMapping kPPCFuncMappings[];

/** Places a function in the recompiled code section, where a stop may redirect a thread. */
#if REX_PLATFORM_MAC
#define REX_RECOMPILED_CODE_SECTION \
  __attribute__((section("__TEXT,__rexguest,regular,pure_instructions")))
#else
#define REX_RECOMPILED_CODE_SECTION __attribute__((section("rexguest")))
#endif

#if REX_PLATFORM_WIN32
/** The PE header of the image that holds the reference. */
extern "C" const char __ImageBase;
#elif REX_PLATFORM_MAC
extern "C" const ::rex::u8 rex_recompiled_code_begin __asm("section$start$__TEXT$__rexguest");
extern "C" const ::rex::u8 rex_recompiled_code_end __asm("section$end$__TEXT$__rexguest");
#else
/** The first byte of the recompiled code section, null when the image has none. */
extern "C" __attribute__((weak, visibility("hidden"))) const ::rex::u8 __start_rexguest[];
/** The byte past the end of the recompiled code section, null when the image has none. */
extern "C" __attribute__((weak, visibility("hidden"))) const ::rex::u8 __stop_rexguest[];
#endif

/**
 * Returns the recompiled code section of the image that expands it.
 *
 * @code
 * rex::ppc::AddRecompiledCode(REX_RECOMPILED_CODE());
 * @endcode
 */
#if REX_PLATFORM_WIN32
#define REX_RECOMPILED_CODE() ::rex::platform::FindImageSection(&__ImageBase, "rexguest")
#elif REX_PLATFORM_MAC
#define REX_RECOMPILED_CODE() \
  ::std::span<const ::rex::u8>(&rex_recompiled_code_begin, &rex_recompiled_code_end)
#else
#define REX_RECOMPILED_CODE() ::std::span<const ::rex::u8>(__start_rexguest, __stop_rexguest)
#endif

namespace rex::ppc {

/**
 * Registers a range of recompiled code. Each registration needs its own RemoveRecompiledCode.
 *
 * @return True when the range is registered, false when all 64 slots are taken.
 */
REX_PPC_API bool AddRecompiledCode(std::span<const u8> code);

/**
 * Removes a registration of a range of recompiled code.
 *
 * @param code The range passed to AddRecompiledCode.
 */
REX_PPC_API void RemoveRecompiledCode(std::span<const u8> code);

/**
 * Reports whether an address is in registered recompiled code. Safe in a signal handler.
 *
 * @return True when a registered range holds the address.
 */
REX_PPC_API bool IsRecompiledCode(u64 address);

}  // namespace rex::ppc
