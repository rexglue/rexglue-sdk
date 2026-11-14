/**
 * @file        tests/unit/hid/xam_input_error_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/xam/input.h>

using rex::kernel::X_RESULT;
using rex::xam::HIDError;

REX_TEST_CASE("Each xam::HIDError maps to its X_ERROR code and back", "[hid]") {
  CHECK(rex::xam::ToXResult(HIDError::kNotConnected) == rex::kernel::X_ERROR_DEVICE_NOT_CONNECTED);
  CHECK(rex::xam::ToXResult(HIDError::kEmpty) == rex::kernel::X_ERROR_EMPTY);
  CHECK(rex::xam::ToXResult(HIDError::kBadArguments) == rex::kernel::X_ERROR_BAD_ARGUMENTS);
  CHECK(rex::xam::ToXResult(HIDError::kFailed) == rex::kernel::X_ERROR_FUNCTION_FAILED);
  for (HIDError code :
       {HIDError::kNotConnected, HIDError::kEmpty, HIDError::kBadArguments, HIDError::kFailed}) {
    CHECK(rex::xam::ToHIDError(rex::xam::ToXResult(code)) == code);
  }
  CHECK(rex::xam::ToHIDError(0x1234) == HIDError::kFailed);
}
