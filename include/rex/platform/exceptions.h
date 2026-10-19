/**
 * @file        platform/exceptions.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Provides cross-platform SEH support for recompiled PPC code.
 *              On Windows, uses native __try/__except (works with MSVC and Clang-cl).
 *              On POSIX, uses signal handlers for SIGSEGV, SIGBUS, SIGFPE and SIGILL.
 *
 * @usage       1. Call rex::InitializeSEH() at startup
 *              2. Generated code wraps SEH scopes with REX_SEH_TRY blocks
 *              3. Hardware exceptions (null deref, access violation) throw SEHException
 *              4. Catch blocks call finally handlers and rethrow
 */

#pragma once

#include <cstdint>
#include <exception>

#include <rex/platform.h>
#include <rex/platform/seh.h>
#include <rex/types.h>

namespace rex {

/** Unwinds a thread that runs recompiled code to its entry when it stops or exits. */
struct ThreadStop {
  /** The exit code the thread ends with. */
  i32 exit_code = 0;
};

/** Thrown when a hardware exception occurs inside an SEH scope on POSIX. */
class SEHException : public std::exception {
 public:
  /** Exception codes, equal to the Windows EXCEPTION_ values. */
  enum Code : u32 {
    kAccessViolation = 0xC0000005,     /**< Invalid memory access. */
    kInPageError = 0xC0000006,         /**< Page could not be loaded, SIGBUS on POSIX. */
    kIllegalInstruction = 0xC000001D,  /**< Invalid instruction. */
    kStackOverflow = 0xC00000FD,       /**< Stack exhausted. */
    kFloatDivideByZero = 0xC000008E,   /**< Floating point divide by 0, SIGFPE on POSIX. */
    kIntegerDivideByZero = 0xC0000094, /**< Integer divide by 0. */
    kUnknown = 0xFFFFFFFF              /**< Any other exception. */
  };

  /**
   * Creates the exception.
   *
   * @param code Exception code.
   * @param address Faulting address, 0 when unknown.
   */
  explicit SEHException(Code code, uintptr_t address = 0) : code_(code), address_(address) {}

  /**
   * Describes the exception.
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

  /**
   * Returns the exception code.
   *
   * @return Exception code.
   */
  Code GetCode() const noexcept { return code_; }

  /**
   * Returns the faulting address.
   *
   * @return Faulting address, 0 when unknown.
   */
  uintptr_t GetAddress() const noexcept { return address_; }

 private:
  Code code_;
  uintptr_t address_;
};

/** Clears the SEH state of the calling thread. */
inline void InitializeSEHThread() {
  platform::GetSEHThreadState() = {};
}

/** Installs the process wide SEH handlers. */
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

/** Closes the SEH scope. */
#define REX_SEH_END }

/** Returns from the function that holds the SEH scope. */
#define REX_SEH_RETURN return

#else

/**
 * Opens an SEH scope. A fault inside it resumes at the handler through sigsetjmp, so the scope adds
 * no C++ cleanup to the function. Pair with REX_SEH_CATCH or REX_SEH_CATCH_ALL and REX_SEH_END, and
 * leave the body early only through REX_SEH_RETURN.
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

/** Closes the SEH scope. */
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
