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

/**
 * Broad classification of an error. Sub-project specific codes go in Error::code.
 */
enum class ErrorCategory {
  NoError,        /**< Success. */
  IO,             /**< File I/O. */
  Memory,         /**< Memory allocation or mapping. */
  Format,         /**< File format parsing (XEX, PE, ELF). */
  Crypto,         /**< Cryptography (decryption, signature). */
  Compression,    /**< Decompression. */
  Runtime,        /**< Runtime execution. */
  Platform,       /**< Platform specific. */
  Config,         /**< Configuration. */
  Validation,     /**< Validation, for example unresolved functions. */
  NotFound,       /**< Resource not found. */
  NotImplemented, /**< Feature not implemented. */
  UserAbort,      /**< User declined an interactive prompt. */
  Module,         /**< Module loading, connection or initialization. */
};

/**
 * An error value carried by rex::Result and rex::Status.
 */
struct Error {
  ErrorCategory category = ErrorCategory::NoError; /**< Classification. */
  std::string message;                             /**< Human readable detail. */
  int code = 0;                                    /**< Platform or library specific code. */

  Error() = default;

  /**
   * @param cat Classification.
   * @param msg Human readable detail.
   * @param err_code Platform or library specific code.
   */
  Error(ErrorCategory cat, std::string msg, int err_code = 0)
      : category(cat), message(std::move(msg)), code(err_code) {}

  /**
   * Builds an error from a system error code.
   *
   * @param cat Classification.
   * @param msg Human readable detail.
   * @param errno_value The errno or platform error code.
   */
  static Error from_errno(ErrorCategory cat, std::string msg, int errno_value) {
    return Error(cat, std::move(msg), errno_value);
  }

  /**
   * @return True when the category is NoError.
   */
  [[nodiscard]] bool is_success() const noexcept { return category == ErrorCategory::NoError; }

  /**
   * @return The message, with the code appended when it is not 0.
   */
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

/**
 * Result type for operations that return nothing on success. Preferred spelling.
 */
using Status = VoidResult;

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
