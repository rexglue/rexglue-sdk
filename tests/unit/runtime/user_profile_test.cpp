/**
 * @file        tests/unit/runtime/user_profile_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <type_traits>

#include <rex/runtime/xam/user_profile.h>

REX_TEST_CASE("A profile setting is destroyed through its base", "[runtime][xam]") {
  CHECK(std::has_virtual_destructor_v<rex::runtime::UserProfile::Setting>);
}
