/**
 * @file        ppc/seh.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Provides cross platform SEH support for recompiled PPC code.
 *              On Windows, uses native __try/__except (works with MSVC and Clang-cl).
 *              On POSIX, uses signal handlers for SIGSEGV, SIGBUS, SIGFPE and SIGILL.
 *
 * @par Usage
 *              1. Call rex::InitializeSEH() at startup
 *              2. Generated code wraps SEH scopes with REX_SEH_TRY blocks
 *              3. Hardware exceptions (null deref, access violation) throw SEHException
 *              4. Catch blocks call finally handlers and rethrow
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <exception>

#include <rex/platform.h>
#include <rex/platform/thread.h>
#include <rex/types.h>
#include <rex/utility.h>

#if !REX_PLATFORM_WIN32
#include <setjmp.h>
#endif

namespace rex::platform {

#if !REX_PLATFORM_WIN32
/** An open SEH scope on POSIX, linked into the chain of its thread. */
struct SEHScope {
  sigjmp_buf env;            /**< Where a fault inside the scope resumes. */
  SEHScope* outer = nullptr; /**< The enclosing scope, null for the outermost. */
  u32 host_depth = 0;        /**< The host call depth when the scope opened. */
};
#endif

struct SEHThreadState {
  u32 code = 0;               /**< Exception code in Windows EXCEPTION_ form. */
  uintptr_t info[2] = {0, 0}; /**< Access type and fault address. */
#if !REX_PLATFORM_WIN32
  SEHScope* top = nullptr;
  u32 host_depth = 0; /**< The number of host frames that recompiled code called into. */
#endif
};

/** Returns the SEH state of the calling thread. */
REX_PPC_API SEHThreadState& GetSEHThreadState();

/**
 * Filter for the Windows __except clause that captures the exception. Always 0 on POSIX.
 *
 * @return Nonzero to run the handler, 0 to continue the search.
 */
REX_PPC_API int SEHFilter(u32 code, void* exception_pointers);

/**
 * Raises the captured exception again. On POSIX it resumes the next open scope or ends the process.
 */
[[noreturn]] REX_PPC_API void SEHRethrow();

/** Installs the SIGSEGV, SIGBUS, SIGFPE and SIGILL handlers once on POSIX. No effect on Windows. */
REX_PPC_API void SEHInitialize();

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
class HostCallDepth : public NonCopyable {
 public:
  HostCallDepth() { ++GetSEHThreadState().host_depth; }
  ~HostCallDepth() { --GetSEHThreadState().host_depth; }
};
#endif

}  // namespace rex::platform

namespace rex {

/** Thrown when a hardware exception occurs inside an SEH scope on POSIX. */
class SEHException : public std::exception {
 public:
  /** Exception codes, equal to the Windows EXCEPTION_ values. */
  enum Code : u32 {
    kAccessViolation = 0xC0000005,
    kInPageError = 0xC0000006, /**< Page could not be loaded, SIGBUS on POSIX. */
    kIllegalInstruction = 0xC000001D,
    kStackOverflow = 0xC00000FD,
    kFloatDivideByZero = 0xC000008E, /**< Floating point divide by 0, SIGFPE on POSIX. */
    kIntegerDivideByZero = 0xC0000094,
    kUnknown = 0xFFFFFFFF
  };

  explicit SEHException(Code code, uintptr_t address = 0) : code_(code), address_(address) {}

  /**
   * Names the exception.
   *
   * @return Static text naming the exception code.
   */
  const char* what() const noexcept override {
    switch (code_) {
      case kAccessViolation:
        return "SEH: Access Violation";
      case kInPageError:
        return "SEH: In-Page Error";
      case kIllegalInstruction:
        return "SEH: Illegal Instruction";
      case kStackOverflow:
        return "SEH: Stack Overflow";
      case kFloatDivideByZero:
        return "SEH: Float Divide by Zero";
      case kIntegerDivideByZero:
        return "SEH: Integer Divide by Zero";
      default:
        return "SEH: Unknown Exception";
    }
  }

  Code GetCode() const noexcept { return code_; }

  /** Returns the faulting address. It is 0 when unknown. */
  uintptr_t GetAddress() const noexcept { return address_; }

 private:
  Code code_;
  uintptr_t address_;
};

inline void InitializeSEHThread() {
  platform::GetSEHThreadState() = {};
}

inline void InitializeSEH() {
  platform::SEHInitialize();
}

#if REX_PLATFORM_WIN32

/**
 * Opens an SEH scope. Pair with REX_SEH_CATCH or REX_SEH_CATCH_ALL and REX_SEH_END.
 *
 * @code
 * REX_SEH_TRY {
 *   body();
 * } REX_SEH_CATCH_ALL {
 *   cleanup();
 *   REX_SEH_RETHROW;
 * } REX_SEH_END
 * @endcode
 */
#define REX_SEH_TRY __try {
/** Closes the REX_SEH_TRY body and opens a handler for the exceptions SEHFilter accepts. */
#define REX_SEH_CATCH \
  }                   \
  __except (::rex::platform::SEHFilter(GetExceptionCode(), GetExceptionInformation())) {
/** Same as REX_SEH_CATCH on Windows. */
#define REX_SEH_CATCH_ALL \
  }                       \
  __except (::rex::platform::SEHFilter(GetExceptionCode(), GetExceptionInformation())) {
/** Raises the captured exception again from inside a handler. */
#define REX_SEH_RETHROW ::rex::platform::SEHRethrow()

#define REX_SEH_END }

/** Returns from the function that holds the SEH scope. */
#define REX_SEH_RETURN return

#else

/**
 * Opens an SEH scope. Pair with REX_SEH_CATCH or REX_SEH_CATCH_ALL and REX_SEH_END, and leave the
 * body early only through REX_SEH_RETURN.
 *
 * @code
 * REX_SEH_TRY {
 *   body();
 * } REX_SEH_CATCH_ALL {
 *   cleanup();
 *   REX_SEH_RETHROW;
 * } REX_SEH_END
 * @endcode
 */
#define REX_SEH_TRY                           \
  {                                           \
    ::rex::platform::SEHScope rex_seh_scope;  \
    ::rex::platform::SEHEnter(rex_seh_scope); \
    if (sigsetjmp(rex_seh_scope.env, 0) == 0)

/** Opens the handler of the scope. */
#define REX_SEH_CATCH else

/** Same as REX_SEH_CATCH on POSIX. */
#define REX_SEH_CATCH_ALL else

/** Passes the captured exception to the next open scope from inside a handler. */
#define REX_SEH_RETHROW ::rex::platform::SEHRethrow()

#define REX_SEH_END                         \
  ::rex::platform::SEHLeave(rex_seh_scope); \
  }

/** Returns from the function that holds the SEH scope. */
#define REX_SEH_RETURN                        \
  do {                                        \
    ::rex::platform::SEHLeave(rex_seh_scope); \
    return;                                   \
  } while (0)

#endif  // REX_PLATFORM_WIN32

}  // namespace rex
