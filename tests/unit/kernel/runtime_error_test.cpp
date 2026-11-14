/**
 * @file        tests/unit/kernel/runtime_error_test.cpp
 * @brief       Error conversions to X_STATUS and X_RESULT
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/filesystem/file_system_error.h>
#include <rex/kernel/types/status.h>

namespace rt = rex::kernel;
using rex::kernel::X_RESULT;
using rex::kernel::X_STATUS;

REX_TEST_CASE("StatusError round trips through ToXStatus", "[kernel][error]") {
  for (const rex::kernel::X_STATUS status :
       {rex::kernel::X_STATUS_INVALID_HANDLE, rex::kernel::X_STATUS_OBJECT_NAME_NOT_FOUND,
        rex::kernel::X_STATUS_OBJECT_NAME_COLLISION, rex::kernel::X_STATUS_NO_MEMORY,
        rex::kernel::X_STATUS_UNSUCCESSFUL}) {
    const rex::Error error = rt::StatusError(status);
    CHECK(error.category == rex::ErrorCategory::kRuntime);
    CHECK(rt::ToXStatus(error) == status);
  }
}

REX_TEST_CASE("XResultError round trips through ToXResult", "[kernel][error]") {
  for (const rex::kernel::X_RESULT result :
       {rex::kernel::X_ERROR_FILE_NOT_FOUND, rex::kernel::X_ERROR_ALREADY_EXISTS,
        rex::kernel::X_ERROR_INVALID_PARAMETER, rex::kernel::X_ERROR_FUNCTION_FAILED}) {
    const rex::Error error = rt::XResultError(result);
    CHECK(error.category == rex::ErrorCategory::kRuntime);
    CHECK(rt::ToXResult(error) == result);
  }
}

REX_TEST_CASE("ToXStatus maps a Result", "[kernel][error]") {
  const rex::Result<u32> value = 5u;
  CHECK(rt::ToXStatus(value) == rex::kernel::X_STATUS_SUCCESS);
  const rex::Result<void> failed = rex::Err(rt::StatusError(rex::kernel::X_STATUS_INVALID_HANDLE));
  CHECK(rt::ToXStatus(failed) == rex::kernel::X_STATUS_INVALID_HANDLE);
  const rex::Result<void> done = rex::Ok();
  CHECK(rt::ToXResult(done) == rex::kernel::X_ERROR_SUCCESS);
}

REX_TEST_CASE("ToXStatus keeps the filesystem mapping", "[kernel][error]") {
  using rex::filesystem::BuildError;
  using rex::filesystem::FileSystemError;
  CHECK(rt::ToXStatus(BuildError(FileSystemError::kNotFound, "a")) ==
        rex::kernel::X_STATUS_OBJECT_NAME_NOT_FOUND);
  CHECK(rt::ToXStatus(BuildError(FileSystemError::kPathNotFound, "a")) ==
        rex::kernel::X_STATUS_OBJECT_PATH_NOT_FOUND);
  CHECK(rt::ToXStatus(BuildError(FileSystemError::kNoMount, "a")) ==
        rex::kernel::X_STATUS_OBJECT_PATH_NOT_FOUND);
  CHECK(rt::ToXStatus(BuildError(FileSystemError::kExists, "a")) ==
        rex::kernel::X_STATUS_OBJECT_NAME_COLLISION);
  CHECK(rt::ToXStatus(BuildError(FileSystemError::kUnsupported, "a"),
                      rex::kernel::X_STATUS_NOT_SUPPORTED) == rex::kernel::X_STATUS_NOT_SUPPORTED);
  CHECK(rt::ToXStatus(BuildError(FileSystemError::kIO, "a")) == rex::kernel::X_STATUS_UNSUCCESSFUL);
  CHECK(rt::ToXStatus(BuildError(FileSystemError::kSharingViolation, "a")) ==
        rex::kernel::X_STATUS_SHARING_VIOLATION);
  CHECK(rt::ToXStatus(BuildError(FileSystemError::kNotSameDevice, "a")) ==
        rex::kernel::X_STATUS_NOT_SAME_DEVICE);
}

REX_TEST_CASE("ToXResult maps another category to the fallback", "[kernel][error]") {
  CHECK(rt::ToXResult(rex::Error(rex::ErrorCategory::kIO, "a")) ==
        rex::kernel::X_ERROR_FUNCTION_FAILED);
  CHECK(rt::ToXResult(rex::Error(rex::ErrorCategory::kIO, "a"),
                      rex::kernel::X_ERROR_ACCESS_DENIED) == rex::kernel::X_ERROR_ACCESS_DENIED);
}
