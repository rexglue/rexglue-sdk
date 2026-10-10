/**
 * @file        tests/unit/core/fat_timestamp_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/filesystem/xcontent.h>

using rex::filesystem::decode_fat_timestamp;

REX_TEST_CASE("A FAT date and time decode to their FILETIME", "[filesystem]") {
  CHECK(decode_fat_timestamp(0x5D43, 0x645C) == 134355044960000000ull);
}

REX_TEST_CASE("The FAT epoch decodes to 1980-01-01", "[filesystem]") {
  CHECK(decode_fat_timestamp(0x0021, 0x0000) == 119600064000000000ull);
}

REX_TEST_CASE("Month 0 carries back to December of the year before", "[filesystem]") {
  CHECK(decode_fat_timestamp(0x0001, 0x0000) == 119573280000000000ull);
}
