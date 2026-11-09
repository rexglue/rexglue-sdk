/**
 * @file        tests/modules/otheridentity_module.cpp
 * @brief       Module of another SDK release that counts calls into it
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform.h>

extern "C" {

REX_DETAIL_EXPORT int rex_test_create_calls = 0;
REX_DETAIL_EXPORT int rex_test_enumerate_calls = 0;

REX_DETAIL_EXPORT const char* ReXBuildIdentity() {
  return "0.0.0-000000000000";
}

REX_DETAIL_EXPORT void* ReXCreateInterface(const char*, unsigned) {
  ++rex_test_create_calls;
  return nullptr;
}

REX_DETAIL_EXPORT unsigned ReXEnumerateInterfaces(void*, unsigned) {
  ++rex_test_enumerate_calls;
  return 0;
}

}  // extern "C"
