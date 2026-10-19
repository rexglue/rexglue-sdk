/**
 * @file        runtime/guest_stack.h
 * @brief       Guest stacks with their guard page and TLS block
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/platform.h>
#include <rex/export.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex::runtime {

class Memory;

/** Bytes in a page of the stack heap, the guard below every guest stack. */
inline constexpr u32 kGuestStackPageSize = 64 * 1024;

/** Smallest thread and kernel stack. */
inline constexpr u32 kMinimumThreadStackSize = 16 * 1024;

/** Smallest fiber stack. */
inline constexpr u32 kMinimumFiberStackSize = 12 * 1024;

/** Title default stack when the process block holds none. */
inline constexpr u32 kDefaultGuestStackSize = 64 * 1024;

/** Address range of a guest stack and the TLS block above it. */
struct GuestStack {
  /** First byte of the allocation, the guard page. */
  u32 alloc_base = 0;
  /** Lowest usable stack address. */
  u32 limit = 0;
  /** Highest stack address, where the TLS block starts. */
  u32 base = 0;
};

/**
 * Resolves the stack size a caller asked for.
 *
 * @param requested Requested stack bytes, 0 for the title default.
 * @param guest_default The title default from the process block, 0 for kDefaultGuestStackSize.
 * @param minimum The smallest size the caller allows.
 * @return The size rounded up to 4 KB and raised to the minimum.
 */
[[nodiscard]] REX_RUNTIME_API u32 ResolveGuestStackSize(u32 requested, u32 guest_default,
                                                        u32 minimum);

/**
 * Resolves the TLS slot count of a process.
 *
 * @param xex_slot_count The XEX slot count, 0 when the header is missing.
 * @return The slot count, at least 32 and at most 256.
 */
[[nodiscard]] REX_RUNTIME_API u32 ResolveTLSSlotCount(u32 xex_slot_count);

/**
 * Allocates a guest stack in the stack range.
 *
 * @param memory Guest memory.
 * @param size Stack bytes from ResolveGuestStackSize.
 * @param tls_size Bytes of TLS block placed above the stack.
 * @param title_memory True when the pages count against the title memory budget.
 * @return The stack, or an error when the range or the budget has no room.
 */
[[nodiscard]] REX_RUNTIME_API Result<GuestStack> AllocateGuestStack(Memory& memory, u32 size,
                                                                    u32 tls_size,
                                                                    bool title_memory);

/**
 * Releases a stack from AllocateGuestStack.
 *
 * @param memory Guest memory.
 * @param alloc_base The first byte of the stack allocation, 0 for none.
 */
REX_RUNTIME_API void FreeGuestStack(Memory& memory, u32 alloc_base);

}  // namespace rex::runtime
