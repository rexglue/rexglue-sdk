/**
 * @file        result.h
 * @brief       Error handling using std::expected
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <expected>
#include <string>
#include <utility>

namespace rex {

//=============================================================================
// Error Categories
//=============================================================================

enum class ErrorCategory {
  NoError,         // No error (success)
  IO,              // File I/O errors
  Memory,          // Memory allocation/mapping errors
  Format,          // File format parsing errors (XEX, PE, ELF)
  Crypto,          // Cryptography errors (decryption, signature)
  Compression,     // Decompression errors
  Runtime,         // Runtime execution errors
  Platform,        // Platform-specific errors
  Config,          // Configuration errors
  Validation,      // Validation errors (e.g., unresolved functions)
  NotFound,        // Resource not found
  NotImplemented,  // Feature not implemented
};

//=============================================================================
// Error Structure
//=============================================================================

struct Error {
  ErrorCategory category = ErrorCategory::NoError;
  std::string message;
  int code = 0;  // Platform or library-specific error code

  Error() = default;

  Error(ErrorCategory cat, std::string msg, int err_code = 0)
      : category(cat), message(std::move(msg)), code(err_code) {}

  // Create from system error code
  static Error from_errno(ErrorCategory cat, std::string msg, int errno_value) {
    return Error(cat, std::move(msg), errno_value);
  }

  // Check if error represents success
  [[nodiscard]] bool is_success() const noexcept { return category == ErrorCategory::NoError; }

  // Get full error description
  [[nodiscard]] std::string what() const {
    if (is_success()) {
      return "Success";
    }
    std::string result = message;
    if (code != 0) {
      result += " (code: " + std::to_string(code) + ")";
    }
    return result;
  }
};

//=============================================================================
// Result Type Aliases
//=============================================================================

/**
 * Result type for operations that can fail
 * Usage:
 *   Result<int> result = some_operation();
 *   if (result) {
 *       int value = *result;  // Success
 *   } else {
 *       Error err = result.error();  // Failure
 *   }
 */
template <typename T>
using Result = std::expected<T, Error>;

/**
 * Result type for operations that return nothing on success
 */
using VoidResult = std::expected<void, Error>;

//=============================================================================
// Helper Functions
//=============================================================================

/**
 * Create a success result
 */
template <typename T>
inline Result<T> Ok(T&& value) {
  return Result<T>(std::forward<T>(value));
}

/**
 * Create a success result for void operations
 */
inline VoidResult Ok() {
  return VoidResult();
}

/**
 * Create an error result
 */
template <typename T = void>
inline auto Err(Error error) {
  if constexpr (std::is_void_v<T>) {
    return VoidResult(std::unexpected(std::move(error)));
  } else {
    return Result<T>(std::unexpected(std::move(error)));
  }
}

/**
 * Create an error result (convenience overload)
 */
template <typename T = void>
inline auto Err(ErrorCategory category, std::string message, int code = 0) {
  return Err<T>(Error(category, std::move(message), code));
}

}  // namespace rex

//=============================================================================
// TRY Macro - Early return on error
//=============================================================================
// Evaluates the expression and returns early if it contains an error.
// The value is extracted and assigned if successful.
//
// Usage:
//   Result<int> get_value();
//   VoidResult do_something() {
//       int value = TRY(get_value());
//       // ... use value ...
//       return rex::Ok();
//   }
//
// For void results:
//   VoidResult validate();
//   VoidResult process() {
//       TRY(validate());  // Returns on error, continues on success
//       return rex::Ok();
//   }
//=============================================================================

#define REX_TRY_CONCAT_IMPL(x, y) x##y
#define REX_TRY_CONCAT(x, y) REX_TRY_CONCAT_IMPL(x, y)

#define TRY(expr)                                                                 \
  ({                                                                              \
    auto REX_TRY_CONCAT(_rex_try_result_, __LINE__) = (expr);                     \
    if (!REX_TRY_CONCAT(_rex_try_result_, __LINE__)) {                            \
      return std::unexpected(REX_TRY_CONCAT(_rex_try_result_, __LINE__).error()); \
    }                                                                             \
    std::move(REX_TRY_CONCAT(_rex_try_result_, __LINE__)).value();                \
  })
