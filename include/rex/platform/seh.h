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

#include <rex/export.h>
#include <rex/platform.h>
#include <rex/types.h>

#if !REX_PLATFORM_WIN32
#include <setjmp.h>
#endif

namespace rex {
class SEHException;
}

namespace rex::platform {

#if !REX_PLATFORM_WIN32
/** 1 open SEH scope on POSIX, linked into the chain of its thread. */
struct SEHScope {
  sigjmp_buf env;            /**< Where a fault inside the scope resumes. */
  SEHScope* outer = nullptr; /**< The enclosing scope, null for the outermost. */
  u32 host_depth = 0;        /**< The host call depth when the scope opened. */
};
#endif

/** Exception details captured for the calling thread. */
struct SEHThreadState {
  u32 code = 0;               /**< Exception code in Windows EXCEPTION_ form. */
  uintptr_t info[2] = {0, 0}; /**< Access type and fault address. */
#if !REX_PLATFORM_WIN32
  SEHScope* top = nullptr; /**< The innermost open scope. */
  u32 host_depth = 0;      /**< The number of host frames that recompiled code called into. */
#endif
};

/**
 * Returns the SEH state of the calling thread.
 *
 * @return Thread local state.
 */
REX_API SEHThreadState& GetSEHThreadState();

/**
 * Filter for the Windows __except clause. Captures the exception into GetSEHThreadState() when
 * the code is handled. Always 0 on POSIX, where the signal handler jumps to the scope instead.
 *
 * @param code Exception code.
 * @param exception_pointers EXCEPTION_POINTERS of the exception.
 * @return Nonzero to run the handler, 0 to continue the search.
 */
REX_API int SEHFilter(u32 code, void* exception_pointers);

/**
 * Raises the exception captured in GetSEHThreadState() again. On POSIX it resumes the next open
 * scope, or ends the process when none may take it.
 */
[[noreturn]] REX_API void SEHRethrow();

/** Installs the SIGSEGV, SIGBUS, SIGFPE and SIGILL handlers once on POSIX. No effect on Windows. */
REX_API void SEHInitialize();

#if !REX_PLATFORM_WIN32
/**
 * Links a scope into the chain of the calling thread.
 *
 * @param scope The scope REX_SEH_TRY opens.
 */
inline void SEHEnter(SEHScope& scope) {
  SEHThreadState& state = GetSEHThreadState();
  scope.outer = state.top;
  scope.host_depth = state.host_depth;
  state.top = &scope;
}

/**
 * Unlinks a scope when it is still the innermost one.
 *
 * @param scope The scope REX_SEH_TRY opened.
 */
inline void SEHLeave(SEHScope& scope) {
  SEHThreadState& state = GetSEHThreadState();
  if (state.top == &scope) {
    state.top = scope.outer;
  }
}

/** Counts a host frame that recompiled code called into for its lifetime. */
class HostCallDepth {
 public:
  HostCallDepth() { ++GetSEHThreadState().host_depth; }
  ~HostCallDepth() { --GetSEHThreadState().host_depth; }
  HostCallDepth(const HostCallDepth&) = delete;
  HostCallDepth& operator=(const HostCallDepth&) = delete;
};
#endif

}  // namespace rex::platform
