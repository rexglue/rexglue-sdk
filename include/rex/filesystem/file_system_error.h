/**
 * @file        filesystem/file_system_error.h
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
enum class FileSystemError : u8 {
  kNone = 0,
  kNotFound,     /**< The last component of the path does not exist. */
  kPathNotFound, /**< A folder before the last component does not exist or is a file. */
  kExists,
  kIsDirectory,
  kNotDirectory,
  kAccessDenied,
  kEndOfFile, /**< A read started at or past the end of the file. */
  kUnsupported,
  kNoMount,
  kMountExists,
  kInvalidPath, /**< The path is malformed or escapes its root. */
  kCorrupt,
  kIO,
  kSharingViolation, /**< Another open handle holds the file with sharing that refuses the open. */
  kNotSameDevice,    /**< A move names a path on another mount. */
};

/**
 * Builds a rex::Error that carries a filesystem code.
 *
 * @return The error. kNotFound, kPathNotFound and kNoMount map to NotFound, kCorrupt to Format,
 * kUnsupported to NotImplemented, the rest to IO.
 */
[[nodiscard]] inline rex::Error BuildError(FileSystemError code, std::string message) {
  ErrorCategory category = ErrorCategory::kIO;
  switch (code) {
    case FileSystemError::kNotFound:
    case FileSystemError::kPathNotFound:
    case FileSystemError::kNoMount:
      category = ErrorCategory::kNotFound;
      break;
    case FileSystemError::kCorrupt:
      category = ErrorCategory::kFormat;
      break;
    case FileSystemError::kUnsupported:
      category = ErrorCategory::kNotImplemented;
      break;
    default:
      break;
  }
  return {category, std::move(message), static_cast<int>(code)};
}

[[nodiscard]] inline FileSystemError CodeOf(const rex::Error& error) {
  return static_cast<FileSystemError>(error.code);
}

}  // namespace rex::filesystem
