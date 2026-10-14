/**
 * @file        result.h
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

#include <rex/types.h>

namespace rex {

/**
 * Broad classification of an error. Specific codes go in Error::code.
 */
enum class ErrorCategory : u8 {
  kNoError,        /**< Success. */
  kIO,             /**< File I/O. */
  kMemory,         /**< Memory allocation or mapping. */
  kFormat,         /**< File format parsing (XEX, PE, ELF). */
  kCrypto,         /**< Cryptography (decryption, signature). */
  kCompression,    /**< Decompression. */
  kRuntime,        /**< Runtime execution. */
  kPlatform,       /**< Platform specific. */
  kConfig,         /**< Configuration. */
  kValidation,     /**< Validation, for example unresolved functions. */
  kNotFound,       /**< Resource not found. */
  kNotImplemented, /**< Feature not implemented. */
  kUserAbort,      /**< User declined an interactive prompt. */
  kModule,         /**< Module loading, connection or initialization. */
};

/**
 * An error value carried by rex::Result.
 */
struct Error {
  ErrorCategory category = ErrorCategory::kNoError; /**< Classification. */
  std::string message;                              /**< Human readable detail. */
  int code = 0;                                     /**< Platform or library specific code. */

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
   * @return The error.
   */
  static Error GetFromErrno(ErrorCategory cat, std::string msg, int errno_value) {
    return Error(cat, std::move(msg), errno_value);
  }

  /**
   * @return True when the category is NoError.
   */
  [[nodiscard]] bool IsSuccess() const noexcept { return category == ErrorCategory::kNoError; }

  /**
   * @return The message, with the code appended when it is not 0.
   */
  [[nodiscard]] std::string GetDescription() const {
    if (IsSuccess()) {
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
 * Value or Error returned by an operation that can fail.
 *
 * @tparam T The success value type, or void.
 *
 * @code
 * Result<u32> result = ReadCount();
 * if (!result) {
 *   REXLOG_ERROR(core, "{}", result.error().GetDescription());
 * }
 * @endcode
 */
template <typename T>
using Result = std::expected<T, Error>;

/**
 * Builds a successful result holding a value.
 *
 * @tparam T The value type.
 * @param value The value, forwarded into the result.
 * @return A Result<T> holding the value.
 */
template <typename T>
[[nodiscard]] inline Result<T> Ok(T&& value) {
  return Result<T>(std::forward<T>(value));
}

/**
 * Builds a successful result with no value.
 *
 * @return A Result<void> holding success.
 */
[[nodiscard]] inline Result<void> Ok() {
  return Result<void>();
}

/**
 * Builds a failed result.
 *
 * @tparam T The success value type of the result, void by default.
 * @param error The error.
 * @return A Result<T> holding the error.
 */
template <typename T = void>
inline auto Err(Error error) {
  if constexpr (std::is_void_v<T>) {
    return Result<void>(std::unexpected(std::move(error)));
  } else {
    return Result<T>(std::unexpected(std::move(error)));
  }
}

/**
 * Builds a failed result from the parts of an Error.
 *
 * @tparam T The success value type of the result, void by default.
 * @param category Classification.
 * @param message Human readable detail.
 * @param code Platform or library specific code.
 * @return A Result<T> holding the error.
 */
template <typename T = void>
inline auto Err(ErrorCategory category, std::string message, int code = 0) {
  return Err<T>(Error(category, std::move(message), code));
}

}  // namespace rex
