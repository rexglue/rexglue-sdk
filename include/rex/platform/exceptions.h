/**
 * @file        platform/exceptions.h
 * @brief       Structured Exception Handling (SEH) support for recompiled code
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

//=============================================================================
// SEH Exception Type
//=============================================================================

/**
 * @brief Exception thrown when hardware exception occurs in SEH-protected code
 */
class SehException : public std::exception {
 public:
  /// Exception codes matching Windows EXCEPTION_* values
  enum Code : u32 {
    ACCESS_VIOLATION = 0xC0000005,
    IN_PAGE_ERROR = 0xC0000006,
    ILLEGAL_INSTRUCTION = 0xC000001D,
    STACK_OVERFLOW = 0xC00000FD,
    FLOAT_DIVIDE_BY_ZERO = 0xC000008E,
    INTEGER_DIVIDE_BY_ZERO = 0xC0000094,
    UNKNOWN = 0xFFFFFFFF
  };

  explicit SehException(Code code, uintptr_t address = 0) : code_(code), address_(address) {}

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

  Code code() const noexcept { return code_; }
  uintptr_t address() const noexcept { return address_; }

 private:
  Code code_;
  uintptr_t address_;
};

//=============================================================================
// Initialization
//=============================================================================

inline void initialize_seh_thread() {
  platform::seh_active() = false;
}

inline void initialize_seh() {
  platform::seh_initialize();
}

//=============================================================================
// SEH Guard Scope and Macros
//=============================================================================

#if REX_PLATFORM_WIN32

class SehGuard {
 public:
  SehGuard() = default;
  ~SehGuard() = default;
  SehGuard(const SehGuard&) = delete;
  SehGuard& operator=(const SehGuard&) = delete;
};

#define SEH_TRY __try {
#define SEH_CATCH \
  }               \
  __except (::rex::platform::seh_filter(GetExceptionCode(), GetExceptionInformation())) {
#define SEH_CATCH_ALL \
  }                   \
  __except (::rex::platform::seh_filter(GetExceptionCode(), GetExceptionInformation())) {
#define SEH_RETHROW ::rex::platform::seh_rethrow()

#define SEH_END }

#else  // !REX_PLATFORM_WIN32

class SehGuard {
 public:
  SehGuard() {
    was_active_ = platform::seh_active();
    platform::seh_active() = true;
  }

  ~SehGuard() { platform::seh_active() = was_active_; }

  SehGuard(const SehGuard&) = delete;
  SehGuard& operator=(const SehGuard&) = delete;

 private:
  bool was_active_ = false;
};

#define SEH_TRY                  \
  {                              \
    ::rex::SehGuard __seh_guard; \
    try

#define SEH_CATCH catch (const ::rex::SehException&)

#define SEH_CATCH_ALL catch (...)

#define SEH_RETHROW throw

#define SEH_END }

#endif  // REX_PLATFORM_WIN32

}  // namespace rex
