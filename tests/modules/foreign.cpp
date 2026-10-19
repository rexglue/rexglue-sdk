/**
 * @file        tests/modules/foreign.cpp
 * @brief       Test module built against another SDK build, or against none
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/module.h>

extern "C" {

#ifdef REX_TEST_FOREIGN_IDENTITY
REX_DETAIL_EXPORT const char* ReXBuildIdentity() {
  return REX_TEST_FOREIGN_IDENTITY;
}
#endif

REX_DETAIL_EXPORT void* ReXCreateInterface(const char*, rex::u32, rex::i32* out_status) {
  if (out_status)
    *out_status = static_cast<rex::i32>(rex::InterfaceStatus::kNotFound);
  return nullptr;
}

REX_DETAIL_EXPORT void ReXEnumerateInterfaces(const rex::InterfaceEntry** out_entries,
                                              rex::u32* out_count) {
  if (out_entries)
    *out_entries = nullptr;
  if (out_count)
    *out_count = 0;
}

}  // extern "C"
