/**
 * @file        runtime/error.cpp
 * @brief       Errors the runtime interfaces return and their platform status mapping
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/runtime/error.h>

#include <format>

#include <rex/filesystem/error.h>

namespace rex::runtime {

rex::Error StatusError(X_STATUS status) {
  return {ErrorCategory::Runtime, std::format("status {:#010x}", status), static_cast<int>(status)};
}

rex::Error ResultError(X_RESULT result) {
  return {ErrorCategory::Runtime, std::format("result {:#010x}", result), static_cast<int>(result)};
}

X_STATUS ToXStatus(const rex::Error& error, X_STATUS unsupported) {
  if (error.category == ErrorCategory::Runtime) {
    return static_cast<X_STATUS>(error.code);
  }
  using filesystem::Error;
  switch (filesystem::CodeOf(error)) {
    case Error::kNotFound:
    case Error::kNoMount:
      return X_STATUS_NO_SUCH_FILE;
    case Error::kExists:
      return X_STATUS_OBJECT_NAME_COLLISION;
    case Error::kIsDirectory:
      return X_STATUS_FILE_IS_A_DIRECTORY;
    case Error::kNotDirectory:
      return X_STATUS_NOT_A_DIRECTORY;
    case Error::kAccessDenied:
      return X_STATUS_ACCESS_DENIED;
    case Error::kEndOfFile:
      return X_STATUS_END_OF_FILE;
    case Error::kUnsupported:
      return unsupported;
    case Error::kInvalidPath:
      return X_STATUS_OBJECT_NAME_INVALID;
    default:
      return X_STATUS_UNSUCCESSFUL;
  }
}

X_RESULT ToXResult(const rex::Error& error, X_RESULT unsupported) {
  return error.category == ErrorCategory::Runtime ? static_cast<X_RESULT>(error.code) : unsupported;
}

}  // namespace rex::runtime
