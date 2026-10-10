/**
 * @file        hid/error.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>

#include <rex/result.h>
#include <rex/runtime/xtypes.h>
#include <rex/types.h>

namespace rex::hid {

/** HID error codes, stored in rex::Error::code. */
enum class Error : u8 {
  NotConnected = 1, /**< No device answers for the user. */
  Empty = 2,        /**< The device has no keystroke queued. */
  BadArguments = 3, /**< The request is malformed. */
  Failed = 4,       /**< The device reported another failure. */
};

/**
 * Builds a rex::Error that carries a HID code.
 *
 * @param code The HID code.
 * @param message Human readable detail.
 * @return The error, in category Platform.
 */
[[nodiscard]] inline rex::Error MakeError(Error code, std::string message) {
  return {ErrorCategory::Platform, std::move(message), static_cast<int>(code)};
}

/**
 * Returns the HID code of an error.
 *
 * @param error An error built by MakeError.
 * @return The code.
 */
[[nodiscard]] inline Error CodeOf(const rex::Error& error) {
  return static_cast<Error>(error.code);
}

/**
 * Returns the HID code a Win32 input result stands for.
 *
 * @param result A failing Win32 code.
 * @return NotConnected, Empty or BadArguments for their codes, Failed for any other.
 */
[[nodiscard]] inline Error ErrorOf(X_RESULT result) {
  switch (result) {
    case X_ERROR_DEVICE_NOT_CONNECTED:
      return Error::NotConnected;
    case X_ERROR_EMPTY:
      return Error::Empty;
    case X_ERROR_BAD_ARGUMENTS:
      return Error::BadArguments;
    default:
      return Error::Failed;
  }
}

/**
 * Returns the Win32 code the XamInput exports report for a HID code.
 *
 * @param code The HID code.
 * @return The Win32 code.
 */
[[nodiscard]] inline X_RESULT ToXResult(Error code) {
  switch (code) {
    case Error::NotConnected:
      return X_ERROR_DEVICE_NOT_CONNECTED;
    case Error::Empty:
      return X_ERROR_EMPTY;
    case Error::BadArguments:
      return X_ERROR_BAD_ARGUMENTS;
    case Error::Failed:
      break;
  }
  return X_ERROR_FUNCTION_FAILED;
}

}  // namespace rex::hid
