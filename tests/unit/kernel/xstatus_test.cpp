/**
 * @file        tests/unit/kernel/xstatus_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/xam/input.h>
#include <rex/kernel/types/status.h>

namespace rex::kernel {
using ThreadState = int;
}

TEST_CASE("the status headers do not include the guest thread state", "[xstatus]") {
  SUCCEED();
}

TEST_CASE("HID errors map to Win32 codes and back for every kind", "[xstatus]") {
  using rex::xam::HIDError;
  CHECK(rex::xam::ToXResult(HIDError::kNotConnected) == rex::kernel::X_ERROR_DEVICE_NOT_CONNECTED);
  CHECK(rex::xam::ToXResult(HIDError::kEmpty) == rex::kernel::X_ERROR_EMPTY);
  CHECK(rex::xam::ToXResult(HIDError::kBadArguments) == rex::kernel::X_ERROR_BAD_ARGUMENTS);
  CHECK(rex::xam::ToXResult(HIDError::kFailed) == rex::kernel::X_ERROR_FUNCTION_FAILED);
  CHECK(rex::xam::ToHIDError(rex::kernel::X_ERROR_DEVICE_NOT_CONNECTED) == HIDError::kNotConnected);
  CHECK(rex::xam::ToHIDError(rex::kernel::X_ERROR_EMPTY) == HIDError::kEmpty);
  CHECK(rex::xam::ToHIDError(rex::kernel::X_ERROR_BAD_ARGUMENTS) == HIDError::kBadArguments);
  CHECK(rex::xam::ToHIDError(rex::kernel::X_ERROR_FUNCTION_FAILED) == HIDError::kFailed);
  CHECK(rex::xam::ToHIDError(rex::kernel::X_ERROR_ACCESS_DENIED) == HIDError::kFailed);
}
