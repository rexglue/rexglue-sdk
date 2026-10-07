/**
 * @file        runtime/error.h
 * @brief       Errors the runtime interfaces return and their platform status mapping
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/macros.h>
#include <rex/result.h>
#include <rex/runtime/xtypes.h>

namespace rex::runtime {

/**
 * Builds the error a runtime interface returns when a platform call fails with an NT status.
 *
 * @param status The NT status the frozen export returns.
 * @return The error, in category Runtime, with the status as its code.
 */
[[nodiscard]] REX_RUNTIME_API rex::Error StatusError(X_STATUS status);

/**
 * Builds the error a runtime interface returns when a platform call fails with a Win32 code.
 *
 * @param result The Win32 code the frozen export returns.
 * @return The error, in category Runtime, with the code as its code.
 */
[[nodiscard]] REX_RUNTIME_API rex::Error ResultError(X_RESULT result);

/**
 * Maps an error to the NT status the kernel reports.
 *
 * @param error An error built by StatusError or by filesystem::MakeError.
 * @param unsupported The status for the filesystem code kUnsupported.
 * @return The code of a Runtime error, otherwise the status of the filesystem code.
 */
[[nodiscard]] REX_RUNTIME_API X_STATUS ToXStatus(const rex::Error& error,
                                                 X_STATUS unsupported = X_STATUS_NOT_IMPLEMENTED);

/**
 * Maps an error to the Win32 code Xam reports.
 *
 * @param error An error built by ResultError.
 * @param unsupported The code for an error of another category.
 * @return The code of a Runtime error, otherwise unsupported.
 */
[[nodiscard]] REX_RUNTIME_API X_RESULT ToXResult(const rex::Error& error,
                                                 X_RESULT unsupported = X_ERROR_FUNCTION_FAILED);

/**
 * Maps a result to the NT status the kernel reports.
 *
 * @tparam T The value type of the result.
 * @param result The result.
 * @return X_STATUS_SUCCESS, or the status of its error.
 */
template <typename T>
[[nodiscard]] X_STATUS ToXStatus(const Result<T>& result) {
  return result ? X_STATUS_SUCCESS : ToXStatus(result.error());
}

/**
 * Maps a result to the Win32 code Xam reports.
 *
 * @tparam T The value type of the result.
 * @param result The result.
 * @return X_ERROR_SUCCESS, or the code of its error.
 */
template <typename T>
[[nodiscard]] X_RESULT ToXResult(const Result<T>& result) {
  return result ? X_ERROR_SUCCESS : ToXResult(result.error());
}

}  // namespace rex::runtime
