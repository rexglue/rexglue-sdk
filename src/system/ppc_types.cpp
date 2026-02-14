/**
 * @file        system/ppc_types.cpp
 * @brief       PPC thread-local state definitions
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ppc/function.h>

namespace rex {

// Thread-local base pointer for guest address computation
// Set by HostToGuestFunction wrapper before calling entry functions
thread_local uint8_t* g_memory_base = nullptr;

// Thread-local current PPC context for GuestToHostFunction
thread_local PPCContext* g_current_ppc_context = nullptr;

}  // namespace rex
