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

#include <span>

#include <rex/export.h>
#include <rex/platform.h>
#include <rex/types.h>

#if REX_PLATFORM_WIN32
#include <rex/platform/dynlib.h>
#endif

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
/** The first byte of the recompiled code section. */
extern "C" const ::rex::u8 rex_recompiled_code_begin __asm("section$start$__TEXT$__rexguest");
/** The byte past the end of the recompiled code section. */
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
 * rex::platform::AddRecompiledCode(REX_RECOMPILED_CODE());
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

namespace rex::platform {

/**
 * Registers a range of recompiled code. A range registered again only counts
 * up, and each registration needs its own RemoveRecompiledCode.
 *
 * @param code The range.
 * @return True when the range is registered, false when all 64 slots are taken.
 */
REX_API bool AddRecompiledCode(std::span<const u8> code);

/**
 * Removes a registration of a range of recompiled code.
 *
 * @param code The range passed to AddRecompiledCode.
 */
REX_API void RemoveRecompiledCode(std::span<const u8> code);

/**
 * Reports whether an address is in registered recompiled code. Safe in a signal handler.
 *
 * @param address The address to test.
 * @return True when a registered range holds the address.
 */
REX_API bool IsRecompiledCode(u64 address);

}  // namespace rex::platform
