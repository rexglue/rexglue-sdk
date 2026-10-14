/**
 * @file        tests/unit/hid/xam_input_error_test.cpp
 * @brief       hid::HIDError maps to the 4 Win32 codes the XamInput exports return
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/hid/error.h>

using rex::X_RESULT;
using rex::hid::HIDError;

REX_TEST_CASE("Each hid::HIDError maps to its X_ERROR code and back", "[hid]") {
  CHECK(rex::hid::ToXResult(HIDError::kNotConnected) == rex::X_ERROR_DEVICE_NOT_CONNECTED);
  CHECK(rex::hid::ToXResult(HIDError::kEmpty) == rex::X_ERROR_EMPTY);
  CHECK(rex::hid::ToXResult(HIDError::kBadArguments) == rex::X_ERROR_BAD_ARGUMENTS);
  CHECK(rex::hid::ToXResult(HIDError::kFailed) == rex::X_ERROR_FUNCTION_FAILED);
  for (HIDError code :
       {HIDError::kNotConnected, HIDError::kEmpty, HIDError::kBadArguments, HIDError::kFailed}) {
    CHECK(rex::hid::ErrorOf(rex::hid::ToXResult(code)) == code);
  }
  CHECK(rex::hid::ErrorOf(0x1234) == HIDError::kFailed);
}
