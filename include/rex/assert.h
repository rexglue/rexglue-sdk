/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <assert.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

#include <rex/platform.h>

namespace rex {

#define static_assert_size(type, size) \
  static_assert(sizeof(type) == size, "bad definition for " #type ": must be " #size " bytes")

// We rely on assert being compiled out in NDEBUG.
#define rex_assert assert

#define __REX_EXPAND(x) x
#define __REX_ARGC(...) \
  __REX_EXPAND(__REX_ARGC_IMPL(__VA_ARGS__, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0))
#define __REX_ARGC_IMPL(x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14, x15, N, ...) N
#define __REX_MACRO_DISPATCH(func, ...) __REX_MACRO_DISPATCH_(func, __REX_ARGC(__VA_ARGS__))
#define __REX_MACRO_DISPATCH_(func, nargs) __REX_MACRO_DISPATCH__(func, nargs)
#define __REX_MACRO_DISPATCH__(func, nargs) func##nargs

/// Assert failure handler that logs the message before aborting
/// @param file     Source file (__FILE__)
/// @param line     Line number (__LINE__)
/// @param expr     Expression that failed (stringified)
/// @param message  User-provided message explaining the failure
[[noreturn]] inline void rex_assert_fail(const char* file, int line, const char* expr,
                                         const char* message) {
  fprintf(stderr, "Assertion failed: %s\n  Expression: %s\n  Location: %s:%d\n", message, expr,
          file, line);
  fflush(stderr);
  std::abort();
}

#define assert_always(...) rex_assert(false)

#define assert_true(...) __REX_MACRO_DISPATCH(assert_true, __VA_ARGS__)(__VA_ARGS__)
#define assert_true1(expr) rex_assert(expr)
#define assert_true2(expr, message)                        \
  do {                                                     \
    if (!(expr)) {                                         \
      rex_assert_fail(__FILE__, __LINE__, #expr, message); \
    }                                                      \
  } while (0)

#define assert_false(...) __REX_MACRO_DISPATCH(assert_false, __VA_ARGS__)(__VA_ARGS__)
#define assert_false1(expr) rex_assert(!(expr))
#define assert_false2(expr, message)                                \
  do {                                                              \
    if (!!(expr)) {                                                 \
      rex_assert_fail(__FILE__, __LINE__, "!(" #expr ")", message); \
    }                                                               \
  } while (0)

#define assert_zero(...) __REX_MACRO_DISPATCH(assert_zero, __VA_ARGS__)(__VA_ARGS__)
#define assert_zero1(expr) rex_assert((expr) == 0)
#define assert_zero2(expr, message)                                     \
  do {                                                                  \
    if ((expr) != 0) {                                                  \
      rex_assert_fail(__FILE__, __LINE__, "(" #expr ") == 0", message); \
    }                                                                   \
  } while (0)

#define assert_not_zero(...) __REX_MACRO_DISPATCH(assert_not_zero, __VA_ARGS__)(__VA_ARGS__)
#define assert_not_zero1(expr) rex_assert((expr) != 0)
#define assert_not_zero2(expr, message)                                 \
  do {                                                                  \
    if ((expr) == 0) {                                                  \
      rex_assert_fail(__FILE__, __LINE__, "(" #expr ") != 0", message); \
    }                                                                   \
  } while (0)

#define assert_null(...) __REX_MACRO_DISPATCH(assert_null, __VA_ARGS__)(__VA_ARGS__)
#define assert_null1(expr) rex_assert((expr) == nullptr)
#define assert_null2(expr, message)                                           \
  do {                                                                        \
    if ((expr) != nullptr) {                                                  \
      rex_assert_fail(__FILE__, __LINE__, "(" #expr ") == nullptr", message); \
    }                                                                         \
  } while (0)

#define assert_not_null(...) __REX_MACRO_DISPATCH(assert_not_null, __VA_ARGS__)(__VA_ARGS__)
#define assert_not_null1(expr) rex_assert((expr) != nullptr)
#define assert_not_null2(expr, message)                                       \
  do {                                                                        \
    if ((expr) == nullptr) {                                                  \
      rex_assert_fail(__FILE__, __LINE__, "(" #expr ") != nullptr", message); \
    }                                                                         \
  } while (0)

#define assert_unhandled_case(variable) assert_always("unhandled switch(" #variable ") case")

/// Marks code as unreachable. Invokes undefined behavior if reached.
/// Use in switch default cases and after exhaustive if-else chains.
#define rex_unreachable() std::unreachable()

// Fatal error that terminates the program with a message
[[noreturn]] inline void FatalError(const char* message) {
  // Print to stderr and terminate
  fprintf(stderr, "FATAL ERROR: %s\n", message);
  fflush(stderr);
  std::abort();
}

[[noreturn]] inline void FatalError(const std::string& message) {
  FatalError(message.c_str());
}

}  // namespace rex
