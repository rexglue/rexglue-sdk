/**
 * @file        tests/unit/runtime/runtime_error_test.cpp
 * @brief       Round trips of the runtime interface errors
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/filesystem/error.h>
#include <rex/runtime/error.h>

namespace rt = rex::runtime;
using rex::X_RESULT;
using rex::X_STATUS;

TEST_CASE("StatusError round trips through ToXStatus", "[runtime][error]") {
  for (const X_STATUS status :
       {rex::X_STATUS_INVALID_HANDLE, rex::X_STATUS_OBJECT_NAME_NOT_FOUND,
        rex::X_STATUS_OBJECT_NAME_COLLISION, rex::X_STATUS_NO_MEMORY, rex::X_STATUS_UNSUCCESSFUL}) {
    const rex::Error error = rt::StatusError(status);
    CHECK(error.category == rex::ErrorCategory::Runtime);
    CHECK(rt::ToXStatus(error) == status);
  }
}

TEST_CASE("ResultError round trips through ToXResult", "[runtime][error]") {
  for (const X_RESULT result : {rex::X_ERROR_FILE_NOT_FOUND, rex::X_ERROR_ALREADY_EXISTS,
                                rex::X_ERROR_INVALID_PARAMETER, rex::X_ERROR_FUNCTION_FAILED}) {
    const rex::Error error = rt::ResultError(result);
    CHECK(error.category == rex::ErrorCategory::Runtime);
    CHECK(rt::ToXResult(error) == result);
  }
}

TEST_CASE("ToXStatus maps a Result", "[runtime][error]") {
  const rex::Result<u32> value = 5u;
  CHECK(rt::ToXStatus(value) == rex::X_STATUS_SUCCESS);
  const rex::Result<void> failed = rex::Err(rt::StatusError(rex::X_STATUS_INVALID_HANDLE));
  CHECK(rt::ToXStatus(failed) == rex::X_STATUS_INVALID_HANDLE);
  const rex::Result<void> done = rex::Ok();
  CHECK(rt::ToXResult(done) == rex::X_ERROR_SUCCESS);
}

TEST_CASE("ToXStatus keeps the filesystem mapping", "[runtime][error]") {
  using rex::filesystem::Error;
  using rex::filesystem::MakeError;
  CHECK(rt::ToXStatus(MakeError(Error::kNotFound, "a")) == rex::X_STATUS_NO_SUCH_FILE);
  CHECK(rt::ToXStatus(MakeError(Error::kExists, "a")) == rex::X_STATUS_OBJECT_NAME_COLLISION);
  CHECK(rt::ToXStatus(MakeError(Error::kUnsupported, "a"), rex::X_STATUS_NOT_SUPPORTED) ==
        rex::X_STATUS_NOT_SUPPORTED);
  CHECK(rt::ToXStatus(MakeError(Error::kIO, "a")) == rex::X_STATUS_UNSUCCESSFUL);
  CHECK(rt::ToXStatus(MakeError(Error::kSharingViolation, "a")) == rex::X_STATUS_SHARING_VIOLATION);
  CHECK(rt::ToXStatus(MakeError(Error::kNotSameDevice, "a")) == rex::X_STATUS_NOT_SAME_DEVICE);
}

TEST_CASE("ToXResult maps another category to the fallback", "[runtime][error]") {
  CHECK(rt::ToXResult(rex::Error(rex::ErrorCategory::IO, "a")) == rex::X_ERROR_FUNCTION_FAILED);
  CHECK(rt::ToXResult(rex::Error(rex::ErrorCategory::IO, "a"), rex::X_ERROR_ACCESS_DENIED) ==
        rex::X_ERROR_ACCESS_DENIED);
}
