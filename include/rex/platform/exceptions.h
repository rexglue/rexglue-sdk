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
 *              On Linux, uses signal handlers for SIGSEGV/SIGBUS.
 *
 * @usage       1. Call rex::initialize_seh() at startup
 *              2. Generated code wraps SEH scopes with try/catch blocks
 *              3. Hardware exceptions (null deref, access violation) throw SehException
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
class SehException : public std::exception {
 public:
  /** Exception codes, equal to the Windows EXCEPTION_ values. */
  enum Code : u32 {
    ACCESS_VIOLATION = 0xC0000005,       /**< Invalid memory access. */
    IN_PAGE_ERROR = 0xC0000006,          /**< Page could not be loaded, SIGBUS on POSIX. */
    ILLEGAL_INSTRUCTION = 0xC000001D,    /**< Invalid instruction. */
    STACK_OVERFLOW = 0xC00000FD,         /**< Stack exhausted. */
    FLOAT_DIVIDE_BY_ZERO = 0xC000008E,   /**< Floating point divide by 0, SIGFPE on POSIX. */
    INTEGER_DIVIDE_BY_ZERO = 0xC0000094, /**< Integer divide by 0. */
    UNKNOWN = 0xFFFFFFFF                 /**< Any other exception. */
  };

  /**
   * Creates the exception.
   *
   * @param code Exception code.
   * @param address Faulting address, 0 when unknown.
   */
  explicit SehException(Code code, uintptr_t address = 0) : code_(code), address_(address) {}

  /**
   * Describes the exception.
   *
   * @return Static text naming the exception code.
   */
  const char* what() const noexcept override {
    switch (code_) {
      case ACCESS_VIOLATION:
        return "SEH: Access Violation";
      case IN_PAGE_ERROR:
        return "SEH: In-Page Error";
      case ILLEGAL_INSTRUCTION:
        return "SEH: Illegal Instruction";
      case STACK_OVERFLOW:
        return "SEH: Stack Overflow";
      case FLOAT_DIVIDE_BY_ZERO:
        return "SEH: Float Divide by Zero";
      case INTEGER_DIVIDE_BY_ZERO:
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
  Code code() const noexcept { return code_; }

  /**
   * Returns the faulting address.
   *
   * @return Faulting address, 0 when unknown.
   */
  uintptr_t address() const noexcept { return address_; }

 private:
  Code code_;
  uintptr_t address_;
};

/** Clears the SEH state of the calling thread. */
inline void initialize_seh_thread() {
  platform::seh_thread_state() = {};
}

/** Installs the process wide SEH handlers. Call once at startup. */
inline void initialize_seh() {
  platform::seh_initialize();
}

#if REX_PLATFORM_WIN32

/**
 * Opens an SEH scope. Pair with SEH_CATCH or SEH_CATCH_ALL and SEH_END.
 *
 * @code
 * SEH_TRY {
 *   body();
 * } SEH_CATCH_ALL {
 *   cleanup();
 *   SEH_RETHROW;
 * } SEH_END
 * @endcode
 */
#define SEH_TRY __try {
/** Closes the SEH_TRY body and opens a handler for the exceptions seh_filter accepts. */
#define SEH_CATCH \
  }               \
  __except (::rex::platform::seh_filter(GetExceptionCode(), GetExceptionInformation())) {
/** Same as SEH_CATCH on Windows. */
#define SEH_CATCH_ALL \
  }                   \
  __except (::rex::platform::seh_filter(GetExceptionCode(), GetExceptionInformation())) {
/** Raises the captured exception again from inside a handler. */
#define SEH_RETHROW ::rex::platform::seh_rethrow()

/** Closes the SEH scope. */
#define SEH_END }

/** Returns from the function that holds the SEH scope. */
#define SEH_RETURN return

#else  // !REX_PLATFORM_WIN32

/**
 * Opens an SEH scope. A fault inside it resumes at the handler through sigsetjmp, so the scope adds
 * no C++ cleanup to the function. Pair with SEH_CATCH or SEH_CATCH_ALL and SEH_END, and leave the
 * body early only through SEH_RETURN.
 *
 * @code
 * SEH_TRY {
 *   body();
 * } SEH_CATCH_ALL {
 *   cleanup();
 *   SEH_RETHROW;
 * } SEH_END
 * @endcode
 */
#define SEH_TRY                                \
  {                                            \
    ::rex::platform::SehScope rex_seh_scope;   \
    ::rex::platform::seh_enter(rex_seh_scope); \
    if (sigsetjmp(rex_seh_scope.env, 0) == 0)

/** Opens the handler of the scope. */
#define SEH_CATCH else

/** Same as SEH_CATCH on POSIX. */
#define SEH_CATCH_ALL else

/** Passes the captured exception to the next open scope from inside a handler. */
#define SEH_RETHROW ::rex::platform::seh_rethrow()

/** Closes the SEH scope. */
#define SEH_END                              \
  ::rex::platform::seh_leave(rex_seh_scope); \
  }

/** Returns from the function that holds the SEH scope. */
#define SEH_RETURN                             \
  do {                                         \
    ::rex::platform::seh_leave(rex_seh_scope); \
    return;                                    \
  } while (0)

#endif  // REX_PLATFORM_WIN32

}  // namespace rex
