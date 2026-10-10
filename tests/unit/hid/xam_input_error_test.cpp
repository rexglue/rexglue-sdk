/**
 * @file        tests/unit/hid/xam_input_error_test.cpp
 * @brief       hid::Error maps to the 4 Win32 codes the XamInput exports return
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
using rex::hid::Error;

REX_TEST_CASE("Each hid::Error maps to its X_ERROR code and back", "[hid]") {
  CHECK(rex::hid::ToXResult(Error::NotConnected) == rex::X_ERROR_DEVICE_NOT_CONNECTED);
  CHECK(rex::hid::ToXResult(Error::Empty) == rex::X_ERROR_EMPTY);
  CHECK(rex::hid::ToXResult(Error::BadArguments) == rex::X_ERROR_BAD_ARGUMENTS);
  CHECK(rex::hid::ToXResult(Error::Failed) == rex::X_ERROR_FUNCTION_FAILED);
  for (Error code : {Error::NotConnected, Error::Empty, Error::BadArguments, Error::Failed}) {
    CHECK(rex::hid::ErrorOf(rex::hid::ToXResult(code)) == code);
  }
  CHECK(rex::hid::ErrorOf(0x1234) == Error::Failed);
}
