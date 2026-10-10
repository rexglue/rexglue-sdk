/**
 * @file        assert.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/assert.h (Ben Vanik, 2014).
 */

#pragma once

#include <assert.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <utility>

#include <rex/platform.h>

namespace rex::detail {

/**
 * Builds the report of a failed assertion.
 *
 * @param file Source file of the assertion.
 * @param line Source line of the assertion.
 * @param expr The failed expression as text.
 * @param message Detail for the failure, empty when the site gave none.
 * @return The expression line, a message line when the message is not empty, and the location.
 */
inline std::string FormatAssertFailure(const char* file, int line, const char* expr,
                                       const char* message) {
  std::string text = std::string("Assertion failed: ") + expr + "\n";
  if (message[0] != '\0') {
    text += std::string("  Message: ") + message + "\n";
  }
  text += std::string("  Location: ") + file + ":" + std::to_string(line) + "\n";
  return text;
}

/**
 * Prints a failed assertion to stderr and aborts.
 *
 * @param file Source file of the assertion.
 * @param line Source line of the assertion.
 * @param expr The failed expression as text.
 * @param message Detail for the failure, empty when the site gave none.
 */
[[noreturn]] inline void AssertFail(const char* file, int line, const char* expr,
                                    const char* message) noexcept {
  try {
    std::fputs(FormatAssertFailure(file, line, expr, message).c_str(), stderr);
  } catch (const std::exception&) {
    std::fprintf(stderr, "Assertion failed: %s\n  Location: %s:%d\n", expr, file, line);
  }
  std::fflush(stderr);
  std::abort();
}

/**
 * Supplies the empty message of an assertion written without one.
 *
 * @return An empty string.
 */
constexpr const char* AssertMessage() {
  return "";
}

/**
 * Passes an assertion message through.
 *
 * @param message Detail for the failure.
 * @return The message.
 */
constexpr const char* AssertMessage(const char* message) {
  return message;
}

}  // namespace rex::detail

/**
 * Checks a condition in Debug. Release does not evaluate the condition.
 *
 * Expands to an unevaluated `decltype` of the condition and the message in Release and to a call
 * of `rex::detail::AssertFail` when the condition is false in Debug.
 *
 * @param cond Condition that must hold.
 * @param ... Optional message.
 *
 * @code
 * REX_ASSERT(count <= capacity, "ring overflow");
 * @endcode
 */
#ifdef NDEBUG
#define REX_ASSERT(cond, ...)       \
  ((void)sizeof(decltype(!(cond))), \
   (void)sizeof(decltype(::rex::detail::AssertMessage(__VA_ARGS__))))
#else
#define REX_ASSERT(cond, ...)                                    \
  ((cond) ? (void)0                                              \
          : ::rex::detail::AssertFail(__FILE__, __LINE__, #cond, \
                                      ::rex::detail::AssertMessage(__VA_ARGS__)))
#endif

/**
 * Aborts in Debug on a path that must not run. Does nothing in Release.
 *
 * @param ... Optional message.
 *
 * @code
 * REX_ASSERT_ALWAYS("unsupported packet");
 * @endcode
 */
#ifdef NDEBUG
#define REX_ASSERT_ALWAYS(...) ((void)sizeof(decltype(::rex::detail::AssertMessage(__VA_ARGS__))))
#else
#define REX_ASSERT_ALWAYS(...)                                 \
  ::rex::detail::AssertFail(__FILE__, __LINE__, "unreachable", \
                            ::rex::detail::AssertMessage(__VA_ARGS__))
#endif

/**
 * Aborts in Debug when a switch reaches a value it does not handle. Does nothing in Release.
 *
 * @param var The switched value.
 *
 * @code
 * default:
 *   REX_UNHANDLED_CASE(format);
 *   break;
 * @endcode
 */
#ifdef NDEBUG
#define REX_UNHANDLED_CASE(var) ((void)0)
#else
#define REX_UNHANDLED_CASE(var) \
  ::rex::detail::AssertFail(__FILE__, __LINE__, "unhandled case: " #var, "")
#endif

/**
 * Checks the size of a type at compile time.
 *
 * Expands to `static_assert(sizeof(type) == size, ...)`.
 *
 * @param type The type.
 * @param size Its required size in bytes.
 *
 * @code
 * REX_STATIC_ASSERT_SIZE(X_KEVENT, 0x10);
 * @endcode
 */
#define REX_STATIC_ASSERT_SIZE(type, size) \
  static_assert(sizeof(type) == (size), "bad definition for " #type ": must be " #size " bytes")

/**
 * Writes a Critical message, flushes every sink and aborts.
 *
 * @param category Category name.
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REX_FATAL(system, "heap {} is corrupted", name);
 * @endcode
 */
#define REX_FATAL(category, fmt, ...)                          \
  do {                                                         \
    REXLOG_CRITICAL(category, fmt __VA_OPT__(, ) __VA_ARGS__); \
    ::rex::log::Flush();                                       \
    ::std::abort();                                            \
  } while (0)

/**
 * Writes a Critical message, flushes every sink and aborts when a condition is false.
 *
 * @param category Category name.
 * @param cond The condition.
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REX_FATAL_IF(system, base != nullptr, "the guest memory base is not mapped");
 * @endcode
 */
#define REX_FATAL_IF(category, cond, fmt, ...)                                               \
  do {                                                                                       \
    if (!(cond)) {                                                                           \
      REXLOG_CRITICAL(category, "check failed: " #cond ". " fmt __VA_OPT__(, ) __VA_ARGS__); \
      ::rex::log::Flush();                                                                   \
      ::std::abort();                                                                        \
    }                                                                                        \
  } while (0)
