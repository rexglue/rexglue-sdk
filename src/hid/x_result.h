/**
 * @file        hid/x_result.h
 * @brief       Win32 input results as hid::HIDError results
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <format>

#include <rex/hid/error.h>
#include <rex/result.h>

namespace rex::hid {

/**
 * Wraps a value read by a call that reports a Win32 input result.
 *
 * @tparam T The value type.
 * @param result The Win32 code of the call.
 * @param value The value the call filled.
 * @return The value, or the HID error of the code.
 */
template <typename T>
[[nodiscard]] Result<T> ToResult(X_RESULT result, const T& value) {
  if (result == X_ERROR_SUCCESS) {
    return value;
  }
  return std::unexpected(
      BuildError(ToHIDError(result), std::format("device result {:08X}", result)));
}

/**
 * Wraps a Win32 input result.
 *
 * @param result The Win32 code.
 * @return Ok, or the HID error of the code.
 */
[[nodiscard]] inline Result<void> ToResult(X_RESULT result) {
  if (result == X_ERROR_SUCCESS) {
    return Ok();
  }
  return std::unexpected(
      BuildError(ToHIDError(result), std::format("device result {:08X}", result)));
}

}  // namespace rex::hid
