/**
 * @file        platform/seh.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <cstdint>

#include <rex/macros.h>
#include <rex/types.h>

namespace rex {
class SehException;
}

namespace rex::platform {

/** Exception details captured for the calling thread. */
struct SehThreadState {
  u32 code = 0;               /**< Exception code in Windows EXCEPTION_ form. */
  uintptr_t info[2] = {0, 0}; /**< Access type and fault address. */
};

/**
 * Returns the SEH state of the calling thread.
 *
 * @return Thread local state.
 */
REX_API SehThreadState& seh_thread_state();

/**
 * Filter for the Windows __except clause. Captures the exception into seh_thread_state() when
 * the code is handled. Always 0 on POSIX, where signal handlers throw instead.
 *
 * @param code Exception code.
 * @param exception_pointers EXCEPTION_POINTERS of the exception.
 * @return Nonzero to run the handler, 0 to continue the search.
 */
REX_API int seh_filter(u32 code, void* exception_pointers);

/** Raises the exception captured in seh_thread_state() again as a host exception or signal. */
[[noreturn]] REX_API void seh_rethrow();

/** Installs the SIGSEGV, SIGBUS, SIGFPE and SIGILL handlers once on POSIX. No effect on Windows. */
REX_API void seh_initialize();

/**
 * Returns the flag that marks the calling thread as inside an SEH scope. POSIX signal handlers
 * throw SehException only while it is set.
 *
 * @return Thread local flag.
 */
REX_API bool& seh_active();

}  // namespace rex::platform
