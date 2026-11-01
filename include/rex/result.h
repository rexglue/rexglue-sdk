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
 * Broad classification of an error. Codes go in Error::code.
 */
enum class ErrorCategory : u8 {
  kNoError,
  kIO,
  kMemory, /**< Memory allocation or mapping. */
  kFormat, /**< File format parsing (XEX, PE, ELF). */
  kCrypto, /**< Cryptography (decryption, signature). */
  kCompression,
  kRuntime, /**< rex::kernel::TitleRuntime execution. */
  kPlatform,
  kConfig,
  kValidation, /**< Validation, for example unresolved functions. */
  kNotFound,
  kNotImplemented,
  kModule, /**< Module loading, connection or initialization. */
};

/**
 * An error value carried by rex::Result.
 */
struct Error {
  ErrorCategory category = ErrorCategory::kNoError;
  std::string message;
  int code = 0; /**< Code from the platform or a library. */

  Error() = default;

  /**
   * Creates an error.
   *
   * @param err_code Code from the platform or a library.
   */
  Error(ErrorCategory cat, std::string msg, int err_code = 0)
      : category(cat), message(std::move(msg)), code(err_code) {}

  static Error FromErrno(ErrorCategory cat, std::string msg, int errno_value) {
    return Error(cat, std::move(msg), errno_value);
  }

  /**
   * Reports whether the error is a success.
   *
   * @return True when the category is kNoError.
   */
  [[nodiscard]] bool IsSuccess() const noexcept { return category == ErrorCategory::kNoError; }

  /**
   * @return "Success" when the category is kNoError, otherwise the message with the code appended
   *         when it is not 0.
   */
  [[nodiscard]] std::string GetDescription() const {
    if (IsSuccess()) {
      return "Success";
    }
    std::string result = message;
    if (code != 0) {
      result += " (code " + std::to_string(code) + ")";
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
 * @param value The value, forwarded into the result.
 */
template <typename T>
[[nodiscard]] inline Result<T> Ok(T&& value) {
  return Result<T>(std::forward<T>(value));
}

/** Builds a successful result with no value. */
[[nodiscard]] inline Result<void> Ok() {
  return Result<void>();
}

/**
 * Builds a failed result.
 *
 * @tparam T The success value type of the result, void by default.
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
 * @param code Code from the platform or a library.
 */
template <typename T = void>
inline auto Err(ErrorCategory category, std::string message, int code = 0) {
  return Err<T>(Error(category, std::move(message), code));
}

}  // namespace rex

/**
 * Pastes two tokens without expanding them first. Helper of REX_TRY_EXPAND_PASTE.
 *
 * The example yields the token _rex_try_12.
 *
 * @code
 * REX_TRY_PASTE(_rex_try_, 12)
 * @endcode
 */
#define REX_TRY_PASTE(a, b) a##b

/**
 * Pastes two tokens after expanding them, so `__LINE__` becomes the line number.
 *
 * @code
 * auto REX_TRY_EXPAND_PASTE(_rex_try_, __LINE__) = Open(path);
 * @endcode
 */
#define REX_TRY_EXPAND_PASTE(a, b) REX_TRY_PASTE(a, b)

/**
 * Unwraps a rex::Result into a declaration, or returns its error from the enclosing function.
 *
 * It is a statement, not an expression.
 *
 * @param decl The declaration that receives the value.
 * @param expr An expression of type rex::Result<T>.
 *
 * @code
 * REX_TRY(auto file, Open(path));
 * @endcode
 */
// NOLINTBEGIN(bugprone-macro-parentheses): decl is a declaration
#define REX_TRY(decl, expr)                                                               \
  auto REX_TRY_EXPAND_PASTE(_rex_try_, __LINE__) = (expr);                                \
  if (!REX_TRY_EXPAND_PASTE(_rex_try_, __LINE__)) {                                       \
    return std::unexpected(std::move(REX_TRY_EXPAND_PASTE(_rex_try_, __LINE__)).error()); \
  }                                                                                       \
  decl = *std::move(REX_TRY_EXPAND_PASTE(_rex_try_, __LINE__))
// NOLINTEND(bugprone-macro-parentheses)

/**
 * Returns the error of a rex::Result expression from the enclosing function.
 *
 * @param expr An expression of type rex::Result<T>, usually rex::Result<void>.
 *
 * @code
 * REX_TRY_VOID(module->Init());
 * @endcode
 */
#define REX_TRY_VOID(expr)                                        \
  do {                                                            \
    auto&& _rex_try_status = (expr);                              \
    if (!_rex_try_status) {                                       \
      return std::unexpected(std::move(_rex_try_status).error()); \
    }                                                             \
  } while (0)

/**
 * Returns an error from the enclosing function when a condition holds.
 *
 * @param error The rex::Error to return when it holds.
 *
 * @code
 * REX_ERROR_IF(ptr == nullptr, rex::Error(rex::ErrorCategory::kValidation, "null"));
 * @endcode
 */
#define REX_ERROR_IF(cond, error)    \
  do {                               \
    if (cond) {                      \
      return std::unexpected(error); \
    }                                \
  } while (0)
