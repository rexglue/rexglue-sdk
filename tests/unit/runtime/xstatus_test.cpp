/**
 * @file        tests/unit/runtime/xstatus_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/hid/error.h>
#include <rex/kernel/xboxkrnl/error.h>
#include <rex/runtime/error.h>
#include <rex/runtime/notifier.h>

namespace rex::runtime {
using ThreadState = int;
}

TEST_CASE("the status headers do not include the guest thread state", "[xstatus]") {
  SUCCEED();
}

TEST_CASE("HID errors map to Win32 codes and back for every kind", "[xstatus]") {
  using rex::hid::HIDError;
  CHECK(rex::hid::ToXResult(HIDError::kNotConnected) == rex::X_ERROR_DEVICE_NOT_CONNECTED);
  CHECK(rex::hid::ToXResult(HIDError::kEmpty) == rex::X_ERROR_EMPTY);
  CHECK(rex::hid::ToXResult(HIDError::kBadArguments) == rex::X_ERROR_BAD_ARGUMENTS);
  CHECK(rex::hid::ToXResult(HIDError::kFailed) == rex::X_ERROR_FUNCTION_FAILED);
  CHECK(rex::hid::ToHIDError(rex::X_ERROR_DEVICE_NOT_CONNECTED) == HIDError::kNotConnected);
  CHECK(rex::hid::ToHIDError(rex::X_ERROR_EMPTY) == HIDError::kEmpty);
  CHECK(rex::hid::ToHIDError(rex::X_ERROR_BAD_ARGUMENTS) == HIDError::kBadArguments);
  CHECK(rex::hid::ToHIDError(rex::X_ERROR_FUNCTION_FAILED) == HIDError::kFailed);
  CHECK(rex::hid::ToHIDError(rex::X_ERROR_ACCESS_DENIED) == HIDError::kFailed);
}
