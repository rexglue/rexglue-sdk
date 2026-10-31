/**
 * @file        tests/unit/runtime/xtypes_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/runtime/xtypes.h>

REX_TEST_CASE("XSUCCEEDED accepts codes whose top bit is clear", "[runtime][xtypes]") {
  CHECK(rex::XSUCCEEDED(rex::X_STATUS_SUCCESS));
  CHECK(rex::XSUCCEEDED(rex::X_STATUS_PENDING));
  CHECK(rex::XSUCCEEDED(rex::X_STATUS_OBJECT_NAME_EXISTS));
  CHECK_FALSE(rex::XSUCCEEDED(rex::X_STATUS_BUFFER_OVERFLOW));
  CHECK_FALSE(rex::XSUCCEEDED(rex::X_STATUS_UNSUCCESSFUL));
  CHECK(rex::XFAILED(rex::X_STATUS_INVALID_PARAMETER));
  CHECK_FALSE(rex::XFAILED(rex::X_STATUS_TIMEOUT));
}

REX_TEST_CASE("XHResultFromWin32 and XResultFromWin32 convert Win32 error codes",
              "[runtime][xtypes]") {
  CHECK(rex::XHResultFromWin32(0) == 0u);
  CHECK(rex::XHResultFromWin32(0x80070057u) == 0x80070057u);
  CHECK(rex::XHResultFromWin32(0x00000057u) == 0x80070057u);
  CHECK(rex::XResultFromWin32(0x00000490u) == 0x00000490u);
}
