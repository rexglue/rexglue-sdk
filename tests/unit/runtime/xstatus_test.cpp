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
  CHECK(rex::hid::ToXResult(rex::hid::HIDError::kEmpty) == rex::X_ERROR_EMPTY);
  CHECK(rex::hid::ToHIDError(rex::X_ERROR_BAD_ARGUMENTS) == rex::hid::HIDError::kBadArguments);
}
