/**
 * @file        filesystem/error.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>
#include <utility>

#include <rex/result.h>
#include <rex/types.h>

namespace rex::filesystem {

/** Filesystem error codes, stored in rex::Error::code. */
enum class Error : u8 {
  kNone = 0,         /**< No error. */
  kNotFound,         /**< The path or 1 of its parents does not exist. */
  kExists,           /**< The path exists and the request needs it absent. */
  kIsDirectory,      /**< A file was required and the path names a directory. */
  kNotDirectory,     /**< A directory was required and the path names a file. */
  kAccessDenied,     /**< The provider refused the operation. */
  kEndOfFile,        /**< A read started at or past the end of the file. */
  kUnsupported,      /**< The provider does not implement the operation. */
  kNoMount,          /**< No mount covers the path. */
  kMountExists,      /**< A mount with the same path exists. */
  kInvalidPath,      /**< The path is malformed or escapes its root. */
  kCorrupt,          /**< A container image failed validation. */
  kIO,               /**< The host reported an I/O failure. */
  kSharingViolation, /**< Another open handle holds the file with sharing that refuses the open. */
  kNotSameDevice,    /**< A move names a path on another mount. */
};

/**
 * Builds a rex::Error that carries a filesystem code.
 *
 * @param code The filesystem code.
 * @param message Human readable detail.
 * @return The error, with the category NotFound for kNotFound and kNoMount, Format for kCorrupt,
 *         NotImplemented for kUnsupported and IO for every other code.
 */
[[nodiscard]] inline rex::Error MakeError(Error code, std::string message) {
  ErrorCategory category = ErrorCategory::IO;
  switch (code) {
    case Error::kNotFound:
    case Error::kNoMount:
      category = ErrorCategory::NotFound;
      break;
    case Error::kCorrupt:
      category = ErrorCategory::Format;
      break;
    case Error::kUnsupported:
      category = ErrorCategory::NotImplemented;
      break;
    default:
      break;
  }
  return {category, std::move(message), static_cast<int>(code)};
}

/**
 * Returns the filesystem code of an error.
 *
 * @param error An error built by MakeError.
 * @return The code.
 */
[[nodiscard]] inline Error CodeOf(const rex::Error& error) {
  return static_cast<Error>(error.code);
}

}  // namespace rex::filesystem
